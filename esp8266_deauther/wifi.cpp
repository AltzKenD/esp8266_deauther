/*
   Copyright (c) 2020 Stefan Kremser (@Spacehuhn)
   This software is licensed under the MIT License. See the license file for details.
   Source: github.com/spacehuhn/esp8266_deauther
 */
 
#include "wifi.h"

extern "C" {
    #include "user_interface.h"
}

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include "language.h"
#include "debug.h"
#include "settings.h"
#include "cli.h"
#include "attack.h"
#include "scan.h"

extern bool progmemToSpiffs(const char* adr, int len, String path);

#include "webfiles.h"

extern Scan scan;
extern CLI cli;
extern Attack attack;

typedef enum wifi_mode_t {
    off = 0,
    ap = 1,
    st = 2
} wifi_mode_t;

typedef struct ap_settings_t {
    char path[33];
    char ssid[33];
    char password[65];
    uint8_t channel;
    bool hidden;
    bool captive_portal;
} ap_settings_t;

namespace wifi {
    // ===== PRIVATE ===== //
    wifi_mode_t mode;
    ap_settings_t ap_settings;

    // Server and other global objects
    AsyncWebServer server(80);
    DNSServer dns;
    IPAddress ip(192, 168, 4, 1);
    IPAddress netmask(255, 255, 255, 0);

    void setPath(String path) {
        if (path.charAt(0) != '/') {
            path = '/' + path;
        }

        if(path.length() > 32) {
            debuglnF("ERROR: Path longer than 32 characters");
        } else {
            strncpy(ap_settings.path, path.c_str(), 32);
        }
    }

    void setSSID(String ssid) {
        if(ssid.length() > 32) {
            debuglnF("ERROR: SSID longer than 32 characters");
        } else {
            strncpy(ap_settings.ssid, ssid.c_str(), 32);
        }
    }

    void setPassword(String password) {
        if(password.length() > 64) {
            debuglnF("ERROR: Password longer than 64 characters");
        } else if (password.length() < 8) {
            debuglnF("ERROR: Password must be at least 8 characters long");
        } else {
            strncpy(ap_settings.password, password.c_str(), 64);
        }
    }
    
    void setChannel(uint8_t ch) {
        if(ch < 1 && ch > 14) {
            debuglnF("ERROR: Channel must be withing the range of 1-14");
        } else {
            ap_settings.channel = ch;
        }
    }

    void setHidden(bool hidden) {
        ap_settings.hidden = hidden;
    }

    void setCaptivePortal(bool captivePortal) {
        ap_settings.captive_portal = captivePortal;
    }
    
    void handleFileList(AsyncWebServerRequest* request) {
        if (!request->hasArg("dir")) {
            request->send(500, "text/plain", "BAD ARGS");
            return;
        }

        String path = request->arg("dir");
        // debugF("handleFileList: ");
        // debugln(path);

        Dir dir = LittleFS.openDir(path);

        String output = String('{'); // {
        File   entry;
        bool   first = true;

        while (dir.next()) {
            entry = dir.openFile("r");

            if (first) first = false;
            else output += ',';                                                 // ,

            output += '[';                                               // [
            output += '"' + entry.name() + '"'; // "filename"
            output += ']';                                              // ]

            entry.close();
        }

        output += '}';
        request->send(200, "application/json", output);
    }

    String getContentType(String filename) {
        //if (server.hasArg("download")) return String(F("application/octet-stream"));
        /*else */if (filename.endsWith(".gz")) filename = filename.substring(0, filename.length() - 3);
        else if (filename.endsWith(".htm")) return "text/html";
        else if (filename.endsWith(".html")) return "text/html";
        else if (filename.endsWith(".css")) return "text/css";
        else if (filename.endsWith(".js")) return "application/javascript";
        else if (filename.endsWith(".png")) return "image/png";
        else if (filename.endsWith(".gif")) return "image/gif";
        else if (filename.endsWith(".jpg")) return "image/jpeg";
        else if (filename.endsWith(".ico")) return "image/x-icon";
        else if (filename.endsWith(".xml")) return "text/xml";
        else if (filename.endsWith(".pdf")) return "application/x-pdf";
        else if (filename.endsWith(".zip")) return "application/x-zip";
        else if (filename.endsWith(".json")) return "application/json";
        else return "text/plain";
    }
    
    bool handleFileRead(AsyncWebServerRequest* request, String path) {
        // prnt(W_AP_REQUEST);
        // prnt(path);

        if (path.charAt(0) != '/') path = '/' + path;
        if (path.charAt(path.length() - 1) == '/') path += String(F("index.html"));

        String contentType = getContentType(path);

        if (!LittleFS.exists(path)) {
            if (LittleFS.exists(path + ".gz")) path += ".gz";
            else if (LittleFS.exists(String(ap_settings.path) + path)) path = String(ap_settings.path) + path;
            else if (LittleFS.exists(String(ap_settings.path) + path + ".gz")) path = String(ap_settings.path) + path + ".gz";
            else {
                // prntln(W_NOT_FOUND);
                return false;
            }
        }

        request->send(LittleFS, path, contentType);
        // prnt(SPACE);
        // prntln(W_OK);

        return true;
    }

    void sendProgmem(AsyncWebServerRequest* request, const char* ptr, size_t size, const char* type) {
        AsyncWebServerResponse* response = request->beginResponse_P(200, type, ptr);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=86400");
        request->send(response);
    }

    // ===== PUBLIC ====== //
    void begin() {
        // Set settings
        setPath("/web");
        setSSID(settings::getAccessPointSettings().ssid);
        setPassword(settings::getAccessPointSettings().password);
        setChannel(settings::getWifiSettings().channel);
        setHidden(settings::getAccessPointSettings().hidden);
        setCaptivePortal(settings::getWebSettings().captive_portal);

        // Set mode
        mode = wifi_mode_t::off;
        WiFi.mode(WIFI_OFF);
        wifi_set_opmode(STATION_MODE);

        // Set mac address
        wifi_set_macaddr(STATION_IF, (uint8_t*)settings::getWifiSettings().mac_st);
        wifi_set_macaddr(SOFTAP_IF, (uint8_t*)settings::getWifiSettings().mac_ap);
    }

    String getMode() {
        switch (mode) {
            case wifi_mode_t::off:
                return "OFF";
            case wifi_mode_t::ap:
                return "AP";
            case wifi_mode_t::st:
                return "STATION";
            default:
                return String();
        }
    }

    void printStatus() {
        prnt(String(F("[WiFi] Path: '")));
        prnt(ap_settings.path);
        prnt(String(F("', Mode: '")));
        prnt(getMode());
        prnt(String(F("', SSID: '")));
        prnt(ap_settings.ssid);
        prnt(String(F("', password: '")));
        prnt(ap_settings.password);
        prnt(String(F("', channel: '")));
        prnt(ap_settings.channel);
        prnt(String(F("', hidden: ")));
        prnt(b2s(ap_settings.hidden));
        prnt(String(F(", captive-portal: ")));
        prntln(b2s(ap_settings.captive_portal));
    }

    void startNewAP(String path, String ssid, String password, uint8_t ch, bool hidden, bool captivePortal) {
        setPath(path);
        setSSID(ssid);
        setPassword(password);
        setChannel(ch);
        setHidden(hidden);
        setCaptivePortal(captivePortal);

        startAP();
    }
/*
    void startAP(String path) {
        setPath(path):

        startAP();
    }
*/
    void startAP() {
        WiFi.softAPConfig(ip, ip, netmask);
        WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);

        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(53, "*", ip);

        MDNS.begin("deauth.me");

        server.on("/list", HTTP_GET, handleFileList); // list directory

        // ================================================================
        // post here the output of the webConverter.py
        #ifdef USE_PROGMEM_WEB_FILES
        if (!settings::getWebSettings().use_spiffs) {
            server.on("/", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, indexhtml, sizeof(indexhtml), W_HTML);
            });
            server.on("/attack.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, attackhtml, sizeof(attackhtml), W_HTML);
            });
            server.on("/index.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, indexhtml, sizeof(indexhtml), W_HTML);
            });
            server.on("/info.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, infohtml, sizeof(infohtml), W_HTML);
            });
            server.on("/scan.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, scanhtml, sizeof(scanhtml), W_HTML);
            });
            server.on("/ap_settings.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, settingshtml, sizeof(settingshtml), W_HTML);
            });
            server.on("/ssids.html", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, ssidshtml, sizeof(ssidshtml), W_HTML);
            });
            server.on("/style.css", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, stylecss, sizeof(stylecss), W_CSS);
            });
            server.on("/js/attack.js", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, attackjs, sizeof(attackjs), W_JS);
            });
            server.on("/js/scan.js", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, scanjs, sizeof(scanjs), W_JS);
            });
            server.on("/js/ap_settings.js", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, settingsjs, sizeof(settingsjs), W_JS);
            });
            server.on("/js/site.js", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, sitejs, sizeof(sitejs), W_JS);
            });
            server.on("/js/ssids.js", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, ssidsjs, sizeof(ssidsjs), W_JS);
            });
            server.on("/lang/cn.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, cnlang, sizeof(cnlang), W_JSON);
            });
            server.on("/lang/cs.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, cslang, sizeof(cslang), W_JSON);
            });
            server.on("/lang/de.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, delang, sizeof(delang), W_JSON);
            });
            server.on("/lang/en.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, enlang, sizeof(enlang), W_JSON);
            });
            server.on("/lang/es.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, eslang, sizeof(eslang), W_JSON);
            });
            server.on("/lang/fi.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, filang, sizeof(filang), W_JSON);
            });
            server.on("/lang/fr.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, frlang, sizeof(frlang), W_JSON);
            });
            server.on("/lang/it.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, itlang, sizeof(itlang), W_JSON);
            });
            server.on("/lang/ru.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, rulang, sizeof(rulang), W_JSON);
            });
            server.on("/lang/tlh.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
                sendProgmem(request, tlhlang, sizeof(tlhlang), W_JSON);
            });
        }
        server.on("/lang/default.lang", HTTP_GET, [] (AsyncWebServerRequest *request) {
            if (!settings::getWebSettings().use_spiffs) {
                if (String(settings::getWebSettings().lang) == String(F("cn"))) sendProgmem(request, cnlang, sizeof(cnlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("cs"))) sendProgmem(request, cslang, sizeof(cslang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("de"))) sendProgmem(request, delang, sizeof(delang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("en"))) sendProgmem(request, enlang, sizeof(enlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("es"))) sendProgmem(request, eslang, sizeof(eslang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("fi"))) sendProgmem(request, filang, sizeof(filang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("fr"))) sendProgmem(request, frlang, sizeof(frlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("it"))) sendProgmem(request, itlang, sizeof(itlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("ru"))) sendProgmem(request, rulang, sizeof(rulang), W_JSON);
                else if (String(settings::getWebSettings().lang) == String(F("tlh"))) sendProgmem(request, tlhlang, sizeof(tlhlang), W_JSON);

                else handleFileRead(request, String(F("/web/lang/")) + String(settings::getWebSettings().lang) + String(F(".lang")));
            } else {
                handleFileRead(request, String(F("/web/lang/")) + String(settings::getWebSettings().lang) + String(F(".lang")));
            }
        });
        #endif /* ifdef USE_PROGMEM_WEB_FILES */
        // ================================================================

        server.on("/run", HTTP_GET, [] (AsyncWebServerRequest* request) {
            request->send(200, "text/plain", "OK");
            String input = request->arg("cmd");
            cli.exec(input);
        });

        server.on("/attack.json", HTTP_GET, [] (AsyncWebServerRequest* request) {
            String json { attack.getStatusJSON() };
            request->send(200, "application/json", json);
        });

        // aggressively caching static assets
        server.serveStatic("/js", LittleFS, "/web/js", "max-age=86400");

        // called when the url is not defined here
        // use it to load content from SPIFFS
        server.onNotFound([] (AsyncWebServerRequest *request) {
            if (!handleFileRead(request, request->url())) {
                request->send(404, "text/plain", "ERROR 404 File Not Found");
                //server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
                //server.send(200, "text/html", indexhtml); 
                //sendProgmem(indexhtml, sizeof(indexhtml), W_HTML);
            }
        });

        server.begin();
        mode = wifi_mode_t::ap;

        prntln(W_STARTED_AP);
        printStatus();
    }

    void stopAP() {
        if (mode == wifi_mode_t::ap) {
            wifi_promiscuous_enable(0);
            WiFi.persistent(false);
            WiFi.disconnect(true);
            wifi_set_opmode(STATION_MODE);
            prntln(W_STOPPED_AP);
            mode = wifi_mode_t::st;
        }
    }

    void resumeAP() {
        if (mode != wifi_mode_t::ap) {
            mode = wifi_mode_t::ap;
            wifi_promiscuous_enable(0);
            WiFi.softAPConfig(ip, ip, netmask);
            WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);
            prntln(W_STARTED_AP);
        }
    }

    void update() {
        if ((mode != wifi_mode_t::off) && !scan.isScanning()) {
            dns.processNextRequest();
        }
    }

}