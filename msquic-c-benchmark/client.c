#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <msquic.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

// Global variables
const QUIC_API_TABLE* MsQuic = NULL;
HQUIC Registration = NULL;
HQUIC Configuration = NULL;
HQUIC Connection = NULL;
HQUIC Stream = NULL;

// Protocol name
const QUIC_BUFFER Alpn = { sizeof("sample") - 1, (uint8_t*)"sample" };

// Default parameters
const char* ServerName = "127.0.0.1";
uint16_t Port = 44882;
uint32_t BufferSize = 1200;
uint32_t TestDuration = 10; // Default test duration is 10 seconds

// Test status
volatile int Running = 1;
volatile int TestTimeExpired = 0;
uint64_t TotalBytesSent = 0;
struct timespec StartTime, EndTime;

// Send window control
#define MAX_PENDING_SENDS (128*1024)
volatile int PendingSends = 0;

// Signal handler function
void SignalHandler(int signal) {
    printf("\nTest interrupted...\n");
    Running = 0;
}

// Timer handler function
void TimerHandler(int signal) {
    printf("Test time expired\n");
    TestTimeExpired = 1;

    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &EndTime);
}

// Parse command line arguments
void ParseCommandLine(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "s:p:b:t:")) != -1) {
        switch (opt) {
            case 's':
                ServerName = optarg;
                break;
            case 'p':
                Port = (uint16_t)atoi(optarg);
                break;
            case 'b':
                BufferSize = (uint32_t)atoi(optarg);
                break;
            case 't':
                TestDuration = (uint32_t)atoi(optarg);
                break;
            default:
                printf("Usage: %s [-s server_ip] [-p port] [-b buffer_size] [-t test_duration]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
}

// Stream callback function
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
ClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // Data sent complete
        free(Event->SEND_COMPLETE.ClientContext);
        __atomic_fetch_sub(&PendingSends, 1, __ATOMIC_SEQ_CST);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        // Data received
        printf("[stream][%p] Data received\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Peer closed send direction
        printf("[stream][%p] Peer closed send\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        // Stream completely closed
        printf("[stream][%p] Stream closed complete\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

// Send data
void SendData(HQUIC Stream) {
    QUIC_STATUS Status;
    uint8_t* SendBufferRaw;
    QUIC_BUFFER* SendBuffer;

    // Allocate send buffer
    SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + BufferSize);
    if (SendBufferRaw == NULL) {
        printf("Memory allocation failed!\n");
        return;
    }
    SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
    SendBuffer->Length = BufferSize;

    // Fill with random data (only fill once, content is irrelevant)
    memset(SendBuffer->Buffer, 0x42, BufferSize);

    // Send data
    Status = MsQuic->StreamSend(
        Stream,
        SendBuffer,
        1,
        QUIC_SEND_FLAG_NONE,
        SendBuffer);

    if (QUIC_FAILED(Status)) {
        printf("StreamSend failed, 0x%x!\n", Status);
        free(SendBufferRaw);
        return;
    }

    __atomic_fetch_add(&PendingSends, 1, __ATOMIC_SEQ_CST);
    TotalBytesSent += BufferSize;
}

// Connection callback function
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
ClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        // Connection established
        printf("Connected to server %s:%d\n", ServerName, Port);
        printf("Starting throughput test, duration %u seconds, buffer size %u bytes\n", TestDuration, BufferSize);

        QUIC_STATUS Status = MsQuic->StreamOpen(
            Connection,
            QUIC_STREAM_OPEN_FLAG_NONE,
            ClientStreamCallback,
            NULL,
            &Stream);

        if (QUIC_FAILED(Status)) {
            printf("StreamOpen failed, 0x%x!\n", Status);
            Running = 0;
            return Status;
        }

        Status = MsQuic->StreamStart(
            Stream,
            QUIC_STREAM_START_FLAG_NONE);

        if (QUIC_FAILED(Status)) {
            printf("StreamStart failed, 0x%x!\n", Status);
            Running = 0;
            return Status;
        }

        // Record start time
        clock_gettime(CLOCK_MONOTONIC, &StartTime);

        // Set timer
        signal(SIGALRM, TimerHandler);
        alarm(TestDuration);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // Connection completely closed
        printf("[conn][%p] Connection closed complete\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        Running = 0;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

int main(int argc, char *argv[]) {
    QUIC_STATUS Status;

    // Parse command line arguments
    ParseCommandLine(argc, argv);

    // Set signal handlers
    signal(SIGINT, SignalHandler);

    // Initialize random number generator
    srand((unsigned int)time(NULL));

    // Open MsQuic
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        return 1;
    }

    // Create registration
    const QUIC_REGISTRATION_CONFIG RegConfig = { "throughput-client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x!\n", Status);
        goto Cleanup;
    }

    // Create configuration
    QUIC_SETTINGS Settings = { 0 };
    Settings.IdleTimeoutMs = 30000; // Increase timeout
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.SendBufferingEnabled = FALSE;
    Settings.IsSet.SendBufferingEnabled = TRUE;
    Settings.MaxBytesPerKey = 1000000000; // Increase max bytes per key
    Settings.IsSet.MaxBytesPerKey = TRUE;
//    Settings.StreamRecvWindowDefault = 16 * 1024 * 1024; // 16MB receive window
//    Settings.IsSet.StreamRecvWindowDefault = TRUE;

    QUIC_CREDENTIAL_CONFIG CredConfig = { 0 };
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        goto Cleanup;
    }

    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        goto Cleanup;
    }

    // Create connection
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, NULL, &Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto Cleanup;
    }

    printf("Connecting to %s:%d...\n", ServerName, Port);

    // Start connection
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, ServerName, Port))) {
        printf("ConnectionStart failed, 0x%x!\n", Status);
        goto Cleanup;
    }

    // Main loop - continue sending data until test time expires
    while (Running && !TestTimeExpired) {
        if (Stream != NULL) {
            if (PendingSends < MAX_PENDING_SENDS) {
                SendData(Stream);
            } else {
                usleep(100); //XXX: improve this
            }
        }
    }

    // If test time expired but stream still exists, close stream
    if (Stream != NULL && TestTimeExpired) {
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        Stream = NULL;
    }

    // Calculate and display throughput
    double ElapsedSeconds = (EndTime.tv_sec - StartTime.tv_sec) +
                           ((EndTime.tv_nsec - StartTime.tv_nsec) / 1000000000.0);
    double Mbps = (TotalBytesSent * 8.0) / (ElapsedSeconds * 1000000.0);

    printf("\nTest results:\n");
    printf("Total sent data: %.2f MB\n", TotalBytesSent / 1000000.0);
    printf("Test duration: %.2f seconds\n", ElapsedSeconds);
    printf("Throughput: %.2f Mbps\n", Mbps);

Cleanup:
    if (Connection != NULL) {
        MsQuic->ConnectionClose(Connection);
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
