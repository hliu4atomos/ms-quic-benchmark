package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"time"

	"github.com/noboruma/go-msquic/pkg/quic"
)

func main() {
	// Parse command line arguments
	serverAddr := flag.String("server", "localhost", "Server address")
	serverPort := flag.Int("port", 8888, "Server port")
	packetSize := flag.Int("size", 1400, "Packet size (bytes)")
	duration := flag.Int("time", 10, "Test duration (seconds)")
	flag.Parse()

	// Create context that can be canceled by interrupt signal
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()

	// Configure client
	config := quic.Config{
		MaxIncomingStreams: 1000,
		MaxIdleTimeout:     60 * time.Second,
		KeepAlivePeriod:    5 * time.Second,
		Alpn:               "go-msquic-test",
	}

	// Connect to server
	serverAddrWithPort := fmt.Sprintf("%s:%d", *serverAddr, *serverPort)
	fmt.Printf("Connecting to server %s...\n", serverAddrWithPort)

	conn, err := quic.DialAddr(ctx, serverAddrWithPort, config)
	if err != nil {
		fmt.Printf("Connection failed: %v\n", err)
		return
	}
	defer conn.Close()

	fmt.Println("Connected to server")

	// Open stream
	stream, err := conn.OpenStream()
	if err != nil {
		fmt.Printf("Failed to open stream: %v\n", err)
		return
	}
	defer stream.Close()

	// Prepare test data packet
	dataPacket := make([]byte, *packetSize)
	for i := range dataPacket {
		dataPacket[i] = byte(i % 256) // Fill with pattern data
	}

	// Create timer to control test duration
	timer := time.NewTimer(time.Duration(*duration) * time.Second)
	defer timer.Stop()

	// Record start time
	startTime := time.Now()

	// Statistics variables
	var totalBytesSent int64
	var packetsSent int64

	// Sending loop
	fmt.Printf("Starting throughput test, duration %d seconds...\n", *duration)
sendLoop:
	for {
		select {
		case <-timer.C:
			// Test time is up
			break sendLoop
		case <-ctx.Done():
			// Interrupt signal received
			fmt.Println("\nTest interrupted")
			break sendLoop
		default:
			// Send data packet
			n, err := stream.Write(dataPacket)
			if err != nil {
				fmt.Printf("Failed to send data: %v\n", err)
				// Brief pause before continuing
				time.Sleep(10 * time.Millisecond)
				continue
			}
			totalBytesSent += int64(n)
			packetsSent++
		}
	}

	// Calculate test results
	elapsedTime := time.Since(startTime).Seconds()
	throughputMbps := float64(totalBytesSent*8) / (elapsedTime * 1000000)

	// Print test results
	fmt.Println("\nTest Results:")
	fmt.Printf("Total data sent: %.2f MB\n", float64(totalBytesSent)/1000000)
	fmt.Printf("Packets sent: %d\n", packetsSent)
	fmt.Printf("Test duration: %.2f seconds\n", elapsedTime)
	fmt.Printf("Throughput: %.2f Mbps\n", throughputMbps)
}
