package main

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/signal"
	"time"

	"github.com/noboruma/go-msquic/pkg/quic"
)

func main() {
	// Create context that can be canceled by interrupt signal
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()

	// Configure server
	config := quic.Config{
		MaxIncomingStreams: 1000,
		CertFile:           "./server.cert",
		KeyFile:            "./server.key",
		Alpn:               "go-msquic-test",
	}

	// Listen address
	fmt.Println("Starting throughput test server, listening on port 8888...")
	listener, err := quic.ListenAddr("0.0.0.0:8888", config)
	if err != nil {
		fmt.Printf("Listen failed: %v\n", err)
		return
	}
	defer listener.Close()

	fmt.Println("Waiting for client connections...")

	// Statistics variables
	var totalConnections int
	var totalBytesReceived int64

	// Connection handler function
	handleConnection := func(conn quic.Connection) {
		defer conn.Close()

		connID := totalConnections
		totalConnections++
		fmt.Printf("Client #%d connected\n", connID)

		// Accept client stream
		stream, err := conn.AcceptStream(ctx)
		if err != nil {
			fmt.Printf("Accept stream failed: %v\n", err)
			return
		}
		defer stream.Close()

		// Read buffer
		buffer := make([]byte, 2048)
		var connectionBytesReceived int64

		// Continuously read and discard data
		for {
			n, err := stream.Read(buffer)
			if err != nil {
				if err != io.EOF {
					fmt.Printf("Failed to read data: %v\n", err)
				}
				break
			}

			// Update statistics
			connectionBytesReceived += int64(n)
			totalBytesReceived += int64(n)

			// Print status every 100MB of data received
			if connectionBytesReceived%(100*1000*1000) < int64(n) {
				fmt.Printf("Client #%d: Received %.2f MB of data\n",
					connID, float64(connectionBytesReceived)/1000000)
			}
		}

		fmt.Printf("Client #%d disconnected, total received %.2f MB of data\n",
			connID, float64(connectionBytesReceived)/1000000)
	}

	// Accept connection loop
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			default:
				conn, err := listener.Accept(ctx)
				if err != nil {
					if ctx.Err() == nil { // Only print errors in non-cancellation cases
						fmt.Printf("Accept connection failed: %v\n", err)
					}
					continue
				}
				go handleConnection(conn)
			}
		}
	}()

	// Periodically print total statistics
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	go func() {
		for {
			select {
			case <-ticker.C:
				fmt.Printf("Total: %d connections, received %.2f MB of data\n",
					totalConnections, float64(totalBytesReceived)/1000000)
			case <-ctx.Done():
				return
			}
		}
	}()

	// Wait for interrupt signal
	<-ctx.Done()
	fmt.Println("\nServer shutting down...")
	fmt.Printf("Total: %d connections, received %.2f MB of data\n",
		totalConnections, float64(totalBytesReceived)/1000000)
}
