#pragma once

// Copy this file to firmware/include/config.local.h and edit for your setup.
// config.local.h is gitignored.

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Gateway mode. Leave GATEWAY_URL empty (or comment it out) to run the
// firmware standalone (no networking except NTP). Set it to your gateway
// base URL to send events to the OpenClaw gateway server.
#define GATEWAY_URL "" // e.g. "http://192.168.1.1:8080" or "https://gw.example.com"
#define GATEWAY_TOKEN ""              // matches gateway's GATEWAY_TOKEN env, if set
#define DEVICE_ID "bedroom"           // identifies this unit in the records
#define GATEWAY_POLL_MS 30000         // how often to refresh state (ms)

// Optional: pin a CA root cert (PEM) when GATEWAY_URL is https. Leave it
// undefined to use setInsecure() (TLS without verification).
// #define GATEWAY_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"

#define NTP_GMT_OFFSET_SEC 0
#define NTP_DST_OFFSET_SEC 0
