# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SV6621)
  target_sources(
    drivers
    PRIVATE sv6621_ap.c
            sv6621_ap_beacon.c
            sv6621_ap_event.c
            sv6621_ap_mlme.c
            sv6621_ap_peer.c
            sv6621_ap_sae.c
            sv6621_ap_wpa.c
            sv6621_command.c
            sv6621_connection.c
            sv6621_core.c
            sv6621_data.c
            sv6621_filter.c
            sv6621_firmware.c
            sv6621_ioctl.c
            sv6621_management.c
            sv6621_memory.c
            sv6621_network.c
            sv6621_offload.c
            sv6621_packet.c
            sv6621_power.c
            sv6621_protocol.c
            sv6621_regulatory.c
            sv6621_rx.c
            sv6621_sae.c
            sv6621_sae_crypto.c
            sv6621_sae_frame.c
            sv6621_sae_group.c
            sv6621_sae_keys.c
            sv6621_scan.c
            sv6621_sched_scan.c
            sv6621_security.c
            sv6621_service.c
            sv6621_signal.c
            sv6621_station.c
            sv6621_stats.c
            sv6621_transport.c
            sv6621_tx.c
            sv6621_wifi.c
            sv6621_wpa_crypto.c
            sv6621_wpa_eapol.c
            sv6621_wpa_handshake.c)
endif()

if(CONFIG_SV6621_BLUETOOTH)
  target_sources(drivers PRIVATE sv6621_bluetooth.c)
endif()
