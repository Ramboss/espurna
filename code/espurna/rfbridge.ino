/*

ITEAD RF BRIDGE MODULE

Copyright (C) 2017 by Xose Pérez <xose dot perez at gmail dot com>

*/

#ifdef ITEAD_SONOFF_RFBRIDGE

// -----------------------------------------------------------------------------
// DEFINITIONS
// -----------------------------------------------------------------------------

#define RF_MESSAGE_SIZE     9
#define RF_CODE_START       0xAA
#define RF_CODE_ACK         0xA0
#define RF_CODE_LEARN       0xA1
#define RF_CODE_LEARN_KO    0xA2
#define RF_CODE_LEARN_OK    0xA3
#define RF_CODE_RFIN        0xA4
#define RF_CODE_RFOUT       0xA5
#define RF_CODE_STOP        0x55

// -----------------------------------------------------------------------------
// GLOBALS TO THE MODULE
// -----------------------------------------------------------------------------

unsigned char _uartbuf[RF_MESSAGE_SIZE+3] = {0};
unsigned char _uartpos = 0;
unsigned char _learnId = 0;
bool _learnStatus = true;
bool _rfbin = false;

// -----------------------------------------------------------------------------
// PRIVATES
// -----------------------------------------------------------------------------

void _rfbAck() {
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending ACK\n"));
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_ACK);
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();
}

void _rfbLearn() {

    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending LEARN\n"));
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_LEARN);
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();

    #if WEB_SUPPORT
        char buffer[100];
        snprintf_P(buffer, sizeof(buffer), PSTR("{\"action\": \"rfbLearn\", \"data\":{\"id\": %d, \"status\": %d}}"), _learnId, _learnStatus ? 1 : 0);
        wsSend(buffer);
    #endif

}

void _rfbSend(byte * message) {
    Serial.println();
    Serial.write(RF_CODE_START);
    Serial.write(RF_CODE_RFOUT);
    for (unsigned char j=0; j<RF_MESSAGE_SIZE; j++) {
        Serial.write(message[j]);
    }
    Serial.write(RF_CODE_STOP);
    Serial.flush();
    Serial.println();
}

void _rfbSend(byte * message, int times) {

    char buffer[RF_MESSAGE_SIZE];
    _rfbToChar(message, buffer);
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Sending MESSAGE '%s' %d time(s)\n"), buffer, times);

    for (int i=0; i<times; i++) {
        if (i>0) {
            unsigned long start = millis();
            while (millis() - start < RF_SEND_DELAY) delay(1);
        }
        _rfbSend(message);
    }

}

void _rfbDecode() {

    byte action = _uartbuf[0];
    char buffer[RF_MESSAGE_SIZE * 2 + 1] = {0};
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Action 0x%02X\n"), action);

    if (action == RF_CODE_LEARN_KO) {
        _rfbAck();
        DEBUG_MSG_P(PSTR("[RFBRIDGE] Learn timeout\n"));
        #if WEB_SUPPORT
            wsSend_P(PSTR("{\"action\": \"rfbTimeout\"}"));
        #endif
    }

    if (action == RF_CODE_LEARN_OK || action == RF_CODE_RFIN) {
        _rfbToChar(&_uartbuf[1], buffer);
        mqttSend(MQTT_TOPIC_RFIN, buffer);
        _rfbAck();
    }

    if (action == RF_CODE_LEARN_OK) {

        DEBUG_MSG_P(PSTR("[RFBRIDGE] Learn success\n"));
        rfbStore(_learnId, _learnStatus, buffer);

        // Websocket update
        #if WEB_SUPPORT
            char wsb[100];
            snprintf_P(wsb, sizeof(wsb), PSTR("{\"rfb\":[{\"id\": %d, \"status\": %d, \"data\": \"%s\"}]}"), _learnId, _learnStatus ? 1 : 0, buffer);
            wsSend(wsb);
        #endif

    }

    if (action == RF_CODE_RFIN) {

        DEBUG_MSG_P(PSTR("[RFBRIDGE] Forward message '%s'\n"), buffer);

        // Look for the code
        unsigned char id, status;
        bool found = false;
        for (id=0; id<relayCount(); id++) {
            for (status=0; status<2; status++) {
                String code = rfbRetrieve(id, status == 1);
                if (code.length()) {
                    if (code.endsWith(&buffer[12])) {
                        found = true;
                        break;
                    }
                }
            }
            if (found) break;
        }
        if (found) {
            _rfbin = true;
            relayStatus(id, status == 1);
        }

    }

}

void _rfbReceive() {

    static bool receiving = false;

    while (Serial.available()) {

        yield();
        byte c = Serial.read();
        //DEBUG_MSG_P(PSTR("[RFBRIDGE] Received 0x%02X\n"), c);

        if (receiving) {
            if (c == RF_CODE_STOP) {
                _rfbDecode();
                receiving = false;
            } else {
                _uartbuf[_uartpos++] = c;
            }
        } else if (c == RF_CODE_START) {
            _uartpos = 0;
            receiving = true;
        }

    }


}

bool _rfbCompare(const char * code1, const char * code2) {
    return strcmp(&code1[12], &code2[12]) == 0;
}

bool _rfbSameOnOff(unsigned char id) {
    return _rfbCompare(rfbRetrieve(id, true).c_str(), rfbRetrieve(id, false).c_str());
}

/*
From an hexa char array ("A220EE...") to a byte array (half the size)
 */
bool _rfbToArray(const char * in, byte * out) {
    if (strlen(in) != RF_MESSAGE_SIZE * 2) return false;
    char tmp[3] = {0};
    for (unsigned char p = 0; p<RF_MESSAGE_SIZE; p++) {
        memcpy(tmp, &in[p*2], 2);
        out[p] = strtol(tmp, NULL, 16);
    }
    return true;
}

/*
From a byte array to an hexa char array ("A220EE...", double the size)
 */
bool _rfbToChar(byte * in, char * out) {
    for (unsigned char p = 0; p<RF_MESSAGE_SIZE; p++) {
        sprintf_P(&out[p*2], PSTR("%02X"), in[p]);
    }
    return true;
}

void _rfbMqttCallback(unsigned int type, const char * topic, const char * payload) {

    if (type == MQTT_CONNECT_EVENT) {
        char buffer[strlen(MQTT_TOPIC_RFLEARN) + 3];
        snprintf_P(buffer, sizeof(buffer), PSTR("%s/+"), MQTT_TOPIC_RFLEARN);
        mqttSubscribe(buffer);
        mqttSubscribe(MQTT_TOPIC_RFOUT);
    }

    if (type == MQTT_MESSAGE_EVENT) {

        // Match topic
        String t = mqttSubtopic((char *) topic);

        // Check if should go into learn mode
        if (t.startsWith(MQTT_TOPIC_RFLEARN)) {

            _learnId = t.substring(strlen(MQTT_TOPIC_RFLEARN)+1).toInt();
            if (_learnId >= relayCount()) {
                DEBUG_MSG_P(PSTR("[RFBRIDGE] Wrong learnID (%d)\n"), _learnId);
                return;
            }
            _learnStatus = (char)payload[0] != '0';
            _rfbLearn();

        }

        if (t.equals(MQTT_TOPIC_RFOUT)) {
            byte message[RF_MESSAGE_SIZE];
            if (_rfbToArray(payload, message)) {
                _rfbSend(message, 1);
            }
        }

    }

}

// -----------------------------------------------------------------------------
// PUBLIC
// -----------------------------------------------------------------------------

void rfbStore(unsigned char id, bool status, const char * code) {
    DEBUG_MSG_P(PSTR("[RFBRIDGE] Storing %d-%s => '%s'\n"), id, status ? "ON" : "OFF", code);
    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%d%s"), id, status ? "on" : "off");
    setSetting(key, code);
}

String rfbRetrieve(unsigned char id, bool status) {
    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%d%s"), id, status ? "on" : "off");
    return getSetting(key);
}

void rfbStatus(unsigned char id, bool status) {
    String value = rfbRetrieve(id, status);
    if (value.length() > 0) {
        bool same = _rfbSameOnOff(id);
        byte message[RF_MESSAGE_SIZE];
        _rfbToArray(value.c_str(), message);
        unsigned char times = RF_SEND_TIMES;
        if (same) times = _rfbin ? 0 : 1;
        _rfbSend(message, times);
    }
}

void rfbLearn(unsigned char id, bool status) {
    _learnId = id;
    _learnStatus = status;
    _rfbLearn();
}

void rfbForget(unsigned char id, bool status) {

    char key[8] = {0};
    snprintf_P(key, sizeof(key), PSTR("rfb%d%s"), id, status ? "on" : "off");
    delSetting(key);

    // Websocket update
    #if WEB_SUPPORT
        char wsb[100];
        snprintf_P(wsb, sizeof(wsb), PSTR("{\"rfb\":[{\"id\": %d, \"status\": %d, \"data\": \"\"}]}"), id, status ? 1 : 0);
        wsSend(wsb);
    #endif

}

// -----------------------------------------------------------------------------
// SETUP & LOOP
// -----------------------------------------------------------------------------

void rfbSetup() {
    mqttRegister(_rfbMqttCallback);
}

void rfbLoop() {
    _rfbReceive();
}

#endif
