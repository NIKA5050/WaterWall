#pragma once
#include "tunnel.h"
#include "common_types.h"
#include "tunnels/shared/trojan/trojan_types.h"
#include "cJSON.h"
//                                                                        <-- dest2
//                                                <------> udp associate  ---> dest
// meant to be tcp -> tls <------>  TrojanServer
//                                                <------> tcp direct tunnel -> dest
//
//
//
//
//




tunnel_t *newTrojanAuthServer(hloop_t **loops, cJSON *settings);