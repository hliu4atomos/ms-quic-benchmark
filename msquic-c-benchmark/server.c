#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msquic.h>
#include <signal.h>
#include <unistd.h>

// Global variables
const QUIC_API_TABLE* MsQuic = NULL;
HQUIC Registration = NULL;
HQUIC Configuration = NULL;
HQUIC Listener = NULL;

// Protocol name
const QUIC_BUFFER Alpn = { sizeof("sample") - 1, (uint8_t*)"sample" };

// Server port
const uint16_t UdpPort = 44882;

// Running flag
volatile int Running = 1;

// Data counter
uint64_t TotalBytesReceived = 0;
const uint64_t GB = (1024 * 1024 * 1024);

// Signal handler to gracefully shutdown
void SignalHandler(int signal) {
    printf("\nShutting down server...\n");
    Running = 0;
}

// Stream callback function
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
ServerStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
) {
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        // Received data
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            TotalBytesReceived += Event->RECEIVE.Buffers[i].Length;
        }

        // Print every 1GB of data received
        if (TotalBytesReceived / GB > (TotalBytesReceived - Event->RECEIVE.TotalBufferLength) / GB) {
            printf("Received %llu GB of data\n", (unsigned long long)(TotalBytesReceived / GB));
        }
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Peer closed send direction
        printf("Client closed send, received %.2f MB of data\n", TotalBytesReceived / 1000000.0);
        MsQuic->StreamShutdown(
            Stream,
            QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,
            0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Stream completely closed
        MsQuic->StreamClose(Stream);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// Connection callback function
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
ServerConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
) {
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        // Connection established
        printf("Client connected\n");
        // Reset counter
        TotalBytesReceived = 0;
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        // Peer started stream
        printf("Client started stream, preparing to receive data\n");
        MsQuic->SetCallbackHandler(
            Event->PEER_STREAM_STARTED.Stream,
            (void*)ServerStreamCallback,
            NULL);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // Connection completely closed
        MsQuic->ConnectionClose(Connection);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// Listener callback function
_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_LISTENER_CALLBACK)
QUIC_STATUS
QUIC_API
ServerListenerCallback(
    _In_ HQUIC Listener,
    _In_opt_ void* Context,
    _Inout_ QUIC_LISTENER_EVENT* Event
) {
    if (Event->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        // Set connection configuration
        QUIC_STATUS Status = MsQuic->ConnectionSetConfiguration(
            Event->NEW_CONNECTION.Connection,
            Configuration);

        if (QUIC_FAILED(Status)) {
            printf("ConnectionSetConfiguration failed, 0x%x\n", Status);
            MsQuic->ConnectionClose(Event->NEW_CONNECTION.Connection);
            return Status;
        }

        // Set connection callback
        MsQuic->SetCallbackHandler(
            Event->NEW_CONNECTION.Connection,
            (void*)ServerConnectionCallback,
            NULL);
    }

    return QUIC_STATUS_SUCCESS;
}

int main() {
    QUIC_STATUS Status;

    // Set up signal handler
    signal(SIGINT, SignalHandler);

    // Open MsQuic
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x\n", Status);
        return 1;
    }

    // Create registration
    const QUIC_REGISTRATION_CONFIG RegConfig = { "throughput-server", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x\n", Status);
        goto Cleanup;
    }

    // Create configuration
    QUIC_SETTINGS Settings = { 0 };
    Settings.IdleTimeoutMs = 30000; // Increase timeout
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    Settings.SendBufferingEnabled = FALSE;
    Settings.IsSet.SendBufferingEnabled = TRUE;
    Settings.MaxBytesPerKey = 1000000000; // Increase max bytes per key
    Settings.IsSet.MaxBytesPerKey = TRUE;
//    Settings.StreamRecvWindowDefault = 16 * 1024 * 1024; // 16MB receive window
//    Settings.IsSet.StreamRecvWindowDefault = TRUE;

    QUIC_CREDENTIAL_CONFIG CredConfig = { 0 };
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;

    // Create certificate file structure
    QUIC_CERTIFICATE_FILE CertFile;
    CertFile.CertificateFile = "server.cert";
    CertFile.PrivateKeyFile = "server.key";

    // Assign certificate file structure to credential config
    CredConfig.CertificateFile = &CertFile;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x\n", Status);
        goto Cleanup;
    }

    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x\n", Status);
        goto Cleanup;
    }

    // Create listener
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallback, NULL, &Listener))) {
        printf("ListenerOpen failed, 0x%x\n", Status);
        goto Cleanup;
    }

    // Set listener address
    QUIC_ADDR Address = { 0 };
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, UdpPort);

    // Start listener
    if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
        printf("ListenerStart failed, 0x%x\n", Status);
        goto Cleanup;
    }

    printf("Throughput test server started, listening on port %d. Press Ctrl+C to stop.\n", UdpPort);

    // Wait until signaled to stop
    while (Running) {
        usleep(100000);
    }

Cleanup:
    if (Listener != NULL) {
        MsQuic->ListenerClose(Listener);
    }
    if (Configuration != NULL) {
        MsQuic->ConfigurationClose(Configuration);
    }
    if (Registration != NULL) {
        MsQuic->RegistrationClose(Registration);
    }

    MsQuicClose(MsQuic);

    return 0;
}
