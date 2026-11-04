// SPDX-License-Identifier: Apache-2.0

// Command nyacloud is the Nyabula central server: device relay, accounts,
// device ownership and usage stats. See docs/protocol/cloud.md.
package main

import (
	"context"
	"crypto/tls"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"nyabula.tech/cloud/internal/httpapi"
	"nyabula.tech/cloud/internal/relay"
	"nyabula.tech/cloud/internal/stats"
	"nyabula.tech/cloud/internal/store"
)

func main() {
	port := flag.Int("port", 8080, "HTTP listen port")
	listen := flag.String("listen", "", "HTTP listen address (overrides -port)")
	console := flag.String("console", "", "built console directory containing index.html")
	tlsCert := flag.String("tls-cert", "", "PEM server certificate chain (requires -tls-key)")
	tlsKey := flag.String("tls-key", "", "PEM server private key (requires -tls-cert)")
	dbPath := flag.String("db", "nyacloud.db", "SQLite database path")
	cors := flag.String("cors-origin", "", "extra allowed CORS origins, comma separated (localhost is always allowed)")
	flag.Parse()
	if (*tlsCert == "") != (*tlsKey == "") {
		log.Fatal("-tls-cert and -tls-key must be supplied together")
	}

	st, err := store.Open(*dbPath)
	if err != nil {
		log.Fatal(err)
	}
	defer st.Close()

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	collector := stats.New(st)
	go collector.Run(ctx)
	registry := relay.New(st, collector)

	var origins []string
	if *cors != "" {
		origins = strings.Split(*cors, ",")
	}
	handler := httpapi.New(st, registry, collector, origins)
	if *console != "" {
		handler, err = consoleHandler(handler, *console)
		if err != nil {
			log.Fatal(err)
		}
	}

	addr := *listen
	if addr == "" {
		addr = fmt.Sprintf(":%d", *port)
	}
	srv := &http.Server{Addr: addr, Handler: handler,
		ReadHeaderTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second,
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12}}
	stopped := make(chan struct{})
	go func() {
		defer close(stopped)
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdown)
		registry.Close()
	}()
	log.Printf("nyacloud listening on %s (db %s)", addr, *dbPath)
	if *tlsCert != "" {
		err = srv.ListenAndServeTLS(*tlsCert, *tlsKey)
	} else {
		err = srv.ListenAndServe()
	}
	if err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
	<-stopped
	collector.Flush()
}
