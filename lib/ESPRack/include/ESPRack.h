#pragma once
#ifndef ESPRACK_H
#define ESPRACK_H

// Umbrella include for consumers. Pulls in the public framework
// surface — Module API, Builder, App, persistence, web layer, secrets.
// Built-in modules (modules/wifi, modules/security, …) are NOT
// included here; consumers explicitly include the modules they want
// to install:
//
//   #include <ESPRack.h>
//   #include <modules/wifi/WiFiModule.h>
//   #include <modules/security/SecurityModule.h>

#include "Module.h"
#include "Builder.h"
#include "App.h"

#include "ConfigManager.h"
#include "ConfigDelegate.h"
#include "FormBuilder.h"
#include "WebManager.h"
#include "WsManager.h"
#include "SecretsVault.h"
#include "SecurityHash.h"
#include "SecurityManager.h"

#endif  // ESPRACK_H
