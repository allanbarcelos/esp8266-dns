// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#include <unity.h>
#include "../test_support.h"
#include "Config.h"
#include "IoTService.h"
#include "MqttService.h"
#include <ArduinoJson.h>

static Logger logger;
static Config cfg;

// ── Broker falso: guarda o que o aparelho manda e entrega o que o broker
// responderia ──────────────────────────────────────────────────────────────
struct Broker : WiFiClient {
    bool        up = false, refuse = false;
    int         connects = 0, stops = 0;
    std::string host;
    uint16_t    port = 0;
    std::string out;            // aparelho → broker
    std::string in;             // broker → aparelho
    size_t      pos = 0;

    int    available() override { return (int)(in.size() - pos); }
    int    read() override { return pos < in.size() ? (uint8_t)in[pos++] : -1; }
    size_t write(const uint8_t* b, size_t n) override { out.append((const char*)b, n); return n; }
    bool   connected() override { return up; }
    int    connect(const char* h, uint16_t p) override {
        connects++;
        host = h;
        port = p;
        if (refuse) return 0;
        up = true;
        out.clear();
        in.clear();
        pos = 0;
        return 1;
    }
    void stop(unsigned int = 0) override { up = false; stops++; }

    void send(const std::string& bytes) { in += bytes; }
};

// Pacote MQTT decodificado do que o aparelho mandou
struct Packet {
    uint8_t     type, flags;
    std::string body;
};

static std::vector<Packet> packets(const std::string& s) {
    std::vector<Packet> v;
    size_t i = 0;
    while (i < s.size()) {
        Packet p;
        p.type = (uint8_t)s[i] >> 4;
        p.flags = (uint8_t)s[i] & 0x0F;
        i++;
        size_t len = 0, shift = 0;
        uint8_t b;
        do {
            b = (uint8_t)s[i++];
            len |= (size_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        p.body = s.substr(i, len);
        i += len;
        v.push_back(p);
    }
    return v;
}

// Campo com 2 bytes de tamanho na posição *at
static std::string field(const std::string& b, size_t& at) {
    size_t n = ((uint8_t)b[at] << 8) | (uint8_t)b[at + 1];
    std::string s = b.substr(at + 2, n);
    at += 2 + n;
    return s;
}

static std::string topicOf(const Packet& p) { size_t at = 0; return field(p.body, at); }
static std::string payloadOf(const Packet& p) { size_t at = 0; field(p.body, at); return p.body.substr(at); }

static const Packet* published(const std::string& out, const char* topic, size_t nth = 0) {
    static std::vector<Packet> keep;
    keep = packets(out);
    for (auto& p : keep)
        if (p.type == 3 && topicOf(p) == topic && nth-- == 0) return &p;
    return nullptr;
}

// Pacote do broker com o tamanho codificado
static std::string pkt(uint8_t first, const std::string& body) {
    std::string s(1, (char)first);
    size_t n = body.size();
    do {
        uint8_t b = n % 128;
        n /= 128;
        s += (char)(b | (n ? 0x80 : 0));
    } while (n);
    return s + body;
}

static std::string str16(const std::string& s) {
    return std::string(1, (char)(s.size() >> 8)) + (char)(s.size() & 0xFF) + s;
}

static const std::string CONNACK_OK = pkt(0x20, std::string("\x00\x00", 2));
static std::string command(const std::string& text, uint8_t first = 0x30) {
    return pkt(first, str16("esp8266iot/00dead/cmd") + text);
}

struct Fix {
    IoTService  iot{cfg, logger};
    Broker      net;
    MqttService mqtt{cfg, logger, iot, net};

    Fix() {
        strlcpy(cfg.mqtt_host, "192.168.1.10", sizeof(cfg.mqtt_host));
        strlcpy(cfg.iot.actuator, "rele", sizeof(cfg.iot.actuator));
        cfg.iot.actuatorPin = 0;
        iot.begin();
    }

    // Conecta e recebe o CONNACK
    void up(unsigned long t = 0) {
        mqtt.tick(t);
        net.send(CONNACK_OK);
        mqtt.tick(t + 1);
    }
};

void setUp() {
    resetMocks();
    logger = Logger();
    cfg = Config{};
}
void tearDown() {}

// ── Endereço do broker ───────────────────────────────────────────────────────

void test_parse_broker() {
    char h[16];
    uint16_t port = 0;
    TEST_ASSERT_TRUE(MqttService::parseBroker("10.0.0.2", h, sizeof(h), port));
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", h);
    TEST_ASSERT_EQUAL(1883, port);
    TEST_ASSERT_TRUE(MqttService::parseBroker("broker:8883", h, sizeof(h), port));
    TEST_ASSERT_EQUAL_STRING("broker", h);
    TEST_ASSERT_EQUAL(8883, port);
    TEST_ASSERT_TRUE(MqttService::parseBroker("b:65535", h, sizeof(h), port));
    TEST_ASSERT_EQUAL(65535, port);

    TEST_ASSERT_FALSE(MqttService::parseBroker("", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker(":1883", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("b:", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("b:0", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("b:65536", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("b:12x", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("b:1:2", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("a b", h, sizeof(h), port));
    TEST_ASSERT_FALSE(MqttService::parseBroker("1234567890123456", h, sizeof(h), port));
    TEST_ASSERT_TRUE(MqttService::parseBroker("123456789012345", h, sizeof(h), port));
}

// ── Conexão ──────────────────────────────────────────────────────────────────

void test_desligado_sem_broker() {
    Fix f;
    cfg.mqtt_host[0] = '\0';
    f.mqtt.tick(0);
    TEST_ASSERT_EQUAL(0, f.net.connects);
    TEST_ASSERT_EQUAL_STRING("desligado", f.mqtt.status());
}

void test_connect_com_usuario_senha_e_testamento() {
    Fix f;
    strlcpy(cfg.mqtt_host, "192.168.1.10:1884", sizeof(cfg.mqtt_host));
    strlcpy(cfg.mqtt_user, "casa", sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass, "s3nha", sizeof(cfg.mqtt_pass));
    f.mqtt.tick(0);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", f.net.host.c_str());
    TEST_ASSERT_EQUAL(1884, f.net.port);
    TEST_ASSERT_EQUAL(MqttService::CONNECT_MS, MockTLS::lastTimeoutMs);
    TEST_ASSERT_EQUAL_STRING("conectando", f.mqtt.status());

    auto v = packets(f.net.out);
    TEST_ASSERT_EQUAL(1, (int)v.size());
    TEST_ASSERT_EQUAL(1, v[0].type);
    const std::string& b = v[0].body;
    size_t at = 0;
    TEST_ASSERT_EQUAL_STRING("MQTT", field(b, at).c_str());
    TEST_ASSERT_EQUAL(4, (uint8_t)b[at]);                     // nível 3.1.1
    TEST_ASSERT_EQUAL_HEX8(0xE6, (uint8_t)b[at + 1]);         // usuário, senha, testamento retido, sessão limpa
    TEST_ASSERT_EQUAL(MqttService::KEEPALIVE_S, ((uint8_t)b[at + 2] << 8) | (uint8_t)b[at + 3]);
    at += 4;
    TEST_ASSERT_EQUAL_STRING("esp8266iot-00dead", field(b, at).c_str());
    TEST_ASSERT_EQUAL_STRING("esp8266iot/00dead/avail", field(b, at).c_str());
    TEST_ASSERT_EQUAL_STRING("offline", field(b, at).c_str());
    TEST_ASSERT_EQUAL_STRING("casa", field(b, at).c_str());
    TEST_ASSERT_EQUAL_STRING("s3nha", field(b, at).c_str());
    TEST_ASSERT_EQUAL(b.size(), at);
}

void test_connect_sem_usuario_ignora_senha() {
    Fix f;
    strlcpy(cfg.mqtt_pass, "s3nha", sizeof(cfg.mqtt_pass));
    f.mqtt.tick(0);
    auto v = packets(f.net.out);
    TEST_ASSERT_EQUAL_HEX8(0x26, (uint8_t)v[0].body[7]);
    TEST_ASSERT_FALSE(contains(v[0].body, "s3nha"));

    // Usuário sem senha
    strlcpy(cfg.mqtt_user, "casa", sizeof(cfg.mqtt_user));
    cfg.mqtt_pass[0] = '\0';
    f.mqtt.reconnect();
    f.mqtt.tick(1);
    v = packets(f.net.out);
    TEST_ASSERT_EQUAL_HEX8(0xA6, (uint8_t)v[0].body[7]);
}

void test_conectado_publica_disponibilidade_assina_e_publica_estado() {
    Fix f;
    f.up();
    TEST_ASSERT_EQUAL_STRING("conectado", f.mqtt.status());
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: conectado"));

    auto v = packets(f.net.out);
    const Packet* avail = published(f.net.out, "esp8266iot/00dead/avail");
    TEST_ASSERT_NOT_NULL(avail);
    TEST_ASSERT_EQUAL_HEX8(0x01, avail->flags);               // retido
    TEST_ASSERT_EQUAL_STRING("online", payloadOf(*avail).c_str());

    bool sub = false;
    for (auto& p : v) {
        if (p.type != 8) continue;
        TEST_ASSERT_EQUAL_HEX8(0x02, p.flags);
        size_t at = 2;                                         // id do pacote
        TEST_ASSERT_EQUAL_STRING("esp8266iot/00dead/cmd", field(p.body, at).c_str());
        TEST_ASSERT_EQUAL(0, (uint8_t)p.body[at]);             // QoS 0
        sub = true;
    }
    TEST_ASSERT_TRUE(sub);

    const Packet* st = published(f.net.out, "esp8266iot/00dead/state");
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL_HEX8(0x01, st->flags);
    char json[384];
    f.iot.stateJson(json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING(json, payloadOf(*st).c_str());
}

void test_broker_recusa() {
    Fix f;
    f.mqtt.tick(0);
    f.net.send(pkt(0x20, std::string("\x00\x05", 2)));
    f.mqtt.tick(1);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: broker recusou (5)"));
    TEST_ASSERT_EQUAL_STRING("conectando", f.mqtt.status());
    TEST_ASSERT_FALSE(f.net.up);

    // CONNACK curto demais também é recusa
    f.mqtt.tick(1 + MqttService::BACKOFF_MIN_S * 1000UL);
    f.net.send(pkt(0x20, ""));
    f.mqtt.tick(2 + MqttService::BACKOFF_MIN_S * 1000UL);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: broker recusou (255)"));
}

void test_broker_sem_resposta() {
    Fix f;
    f.mqtt.tick(0);
    f.mqtt.tick(MqttService::CONNACK_MS);
    TEST_ASSERT_TRUE(f.net.up);
    f.mqtt.tick(MqttService::CONNACK_MS + 1);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: broker sem resposta"));
    TEST_ASSERT_FALSE(f.net.up);
}

void test_falhas_esperam_cada_vez_mais() {
    Fix f;
    f.net.refuse = true;
    f.mqtt.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: sem conexao com 192.168.1.10"));
    unsigned long t = 0, wait = MqttService::BACKOFF_MIN_S * 1000UL;
    int expected[] = {5, 10, 20, 40, 80, 160, 300, 300};
    for (int s : expected) {
        TEST_ASSERT_EQUAL(s * 1000UL, wait);
        int c = f.net.connects;
        f.mqtt.tick(t + wait - 1);
        TEST_ASSERT_EQUAL(c, f.net.connects);
        f.mqtt.tick(t + wait);
        TEST_ASSERT_EQUAL(c + 1, f.net.connects);
        t += wait;
        wait = (unsigned long)(s < 150 ? s * 2 : 300) * 1000UL;
    }

    // Conectar zera a espera
    f.net.refuse = false;
    f.up(t + 300000UL);
    f.net.up = false;
    f.mqtt.tick(t + 300002UL);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: conexao encerrada"));
    int c = f.net.connects;
    f.mqtt.tick(t + 300002UL + MqttService::BACKOFF_MIN_S * 1000UL);
    TEST_ASSERT_EQUAL(c + 1, f.net.connects);
}

void test_broker_invalido() {
    Fix f;
    strlcpy(cfg.mqtt_host, "a b", sizeof(cfg.mqtt_host));
    f.mqtt.tick(0);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: broker invalido"));
    TEST_ASSERT_EQUAL(0, f.net.connects);
}

// ── Comandos ─────────────────────────────────────────────────────────────────

void test_comando_aciona_e_publica_o_estado() {
    Fix f;
    f.up();
    f.net.out.clear();
    f.net.send(command("ligar"));
    f.mqtt.tick(10);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: comando \"ligar\""));
    const Packet* st = published(f.net.out, "esp8266iot/00dead/state");
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_TRUE(contains(payloadOf(*st), "\"on\":true"));

    f.net.send(command("voar"));
    f.mqtt.tick(11);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: comando \"voar\" recusado"));

    // Comando mais longo que o buffer é cortado (e recusado)
    f.net.send(command(std::string(MqttService::CMD_LEN + 5, 'x')));
    f.mqtt.tick(12);
    TEST_ASSERT_TRUE(logHas(logger, "recusado"));
}

void test_comandos_que_nao_valem() {
    Fix f;
    f.up();
    // Retido: seria repetido a cada conexão
    f.net.send(command("ligar", 0x31));
    f.mqtt.tick(10);
    TEST_ASSERT_FALSE(f.iot.actuatorOn());
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: comando retido ignorado"));

    // QoS 1 (com id do pacote): fora do protocolo usado
    f.net.send(pkt(0x32, str16("esp8266iot/00dead/cmd") + std::string("\x00\x07", 2) + "ligar"));
    // Maior que o buffer: lido e descartado, o seguinte ainda vale
    f.net.send(pkt(0x30, str16("esp8266iot/00dead/cmd") + std::string(200, 'l')));
    // Tópico declarado maior que o pacote
    f.net.send(pkt(0x30, std::string("\x00\x50", 2) + "ab"));
    // Pacote curto demais
    f.net.send(pkt(0x30, "a"));
    for (int i = 0; i < 3; i++) f.mqtt.tick(11);       // até 256 bytes por tick
    TEST_ASSERT_EQUAL(0, f.net.available());
    TEST_ASSERT_FALSE(f.iot.actuatorOn());

    f.net.send(command("ligar"));
    f.mqtt.tick(12);
    TEST_ASSERT_TRUE(f.iot.actuatorOn());
}

void test_pacotes_fora_de_hora_e_tamanho_longo() {
    Fix f;
    f.mqtt.tick(0);
    // Comando antes do CONNACK: ignorado
    f.net.send(command("ligar"));
    f.mqtt.tick(1);
    TEST_ASSERT_FALSE(f.iot.actuatorOn());
    f.net.send(CONNACK_OK);
    f.mqtt.tick(2);
    TEST_ASSERT_EQUAL_STRING("conectado", f.mqtt.status());

    // CONNACK repetido, PINGRESP e SUBACK: nada muda
    f.net.send(CONNACK_OK);
    f.net.send(pkt(0xD0, ""));
    f.net.send(pkt(0x90, std::string("\x00\x01\x00", 3)));
    f.mqtt.tick(3);
    TEST_ASSERT_EQUAL_STRING("conectado", f.mqtt.status());

    // Tamanho com mais de 4 bytes: fora do protocolo, reconecta
    f.net.send(std::string("\xE0\xFF\xFF\xFF\xFF", 5));
    f.mqtt.tick(4);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: pacote invalido"));
    TEST_ASSERT_FALSE(f.net.up);
    TEST_ASSERT_EQUAL_STRING("conectando", f.mqtt.status());
}

void test_leitura_limitada_por_tick() {
    Fix f;
    f.up();
    std::string pings;                           // PINGRESP: 2 bytes cada
    for (size_t i = 0; i < (MqttService::READ_PER_TICK + 10) / 2; i++) pings += pkt(0xD0, "");
    f.net.send(pings);
    f.mqtt.tick(10);
    TEST_ASSERT_EQUAL(10, f.net.available());
}

// ── Estado ───────────────────────────────────────────────────────────────────

void test_estado_so_sai_quando_muda() {
    Fix f;
    f.up();
    f.net.out.clear();
    f.mqtt.tick(MqttService::STATE_MS + 1);
    TEST_ASSERT_NULL(published(f.net.out, "esp8266iot/00dead/state"));

    f.iot.command("ligar", 100);                 // pelo painel ou pela API
    f.mqtt.tick(MqttService::STATE_MS + 2);      // ainda dentro do intervalo
    TEST_ASSERT_NULL(published(f.net.out, "esp8266iot/00dead/state"));
    f.mqtt.tick(2 * MqttService::STATE_MS + 1);
    TEST_ASSERT_NOT_NULL(published(f.net.out, "esp8266iot/00dead/state"));
}

// TelePeriod: sem mudança, o estado sai de novo a cada TELE_MS, contado
// da última publicação
void test_estado_republicado_periodicamente() {
    Fix f;
    f.up(1000);                                  // estado publicado em 1001
    const char* topic = "esp8266iot/00dead/state";
    // Um tick a cada 30 s; o broker responde ao ping e a conexão segue viva
    unsigned long t = 0;
    auto until = [&](unsigned long end) {
        f.net.out.clear();
        while (t < end) {
            t += 30000;
            f.net.send(pkt(0xD0, ""));
            f.mqtt.tick(t);
        }
        return published(f.net.out, topic);
    };
    TEST_ASSERT_NULL(until(300000));             // 1001 + 5 min ainda não passou
    const Packet* st = until(330000);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL(1, st->flags & 1);         // retido

    // Uma mudança no meio reinicia a conta
    TEST_ASSERT_NULL(until(450000));
    f.iot.command("ligar", 450000);
    TEST_ASSERT_NOT_NULL(until(480000));
    TEST_ASSERT_NULL(until(480000 + MqttService::TELE_MS - 30000));
    TEST_ASSERT_NOT_NULL(until(480000 + MqttService::TELE_MS));
    TEST_ASSERT_TRUE(f.net.up);
}

void test_estado_longo_usa_tamanho_de_dois_bytes() {
    Fix f;
    strlcpy(cfg.iot.sensor, "digital", sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = 2;
    f.iot.begin();
    f.up();
    const Packet* st = published(f.net.out, "esp8266iot/00dead/state");
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_TRUE(st->body.size() > 127);
}

// ── Keepalive e queda ────────────────────────────────────────────────────────

void test_ping_e_broker_parado() {
    Fix f;
    f.up();
    f.net.out.clear();
    unsigned long half = MqttService::KEEPALIVE_S * 500UL;
    f.mqtt.tick(half);                           // < metade desde o último envio
    TEST_ASSERT_TRUE(f.net.out.empty());
    f.mqtt.tick(half + 1);
    TEST_ASSERT_EQUAL_STRING(std::string("\xC0\x00", 2).c_str(), f.net.out.c_str());
    TEST_ASSERT_EQUAL(2, (int)f.net.out.size());

    // PINGRESP mantém viva
    f.net.send(pkt(0xD0, ""));
    f.mqtt.tick(2 * half);
    TEST_ASSERT_TRUE(f.net.up);

    unsigned long dead = 2 * half + MqttService::KEEPALIVE_S * 1500UL;
    f.mqtt.tick(dead);
    TEST_ASSERT_TRUE(f.net.up);
    f.mqtt.tick(dead + 1);
    TEST_ASSERT_TRUE(logHas(logger, "MQTT: broker parado"));
    TEST_ASSERT_FALSE(f.net.up);
}

void test_pausa_wifi_e_reconexao() {
    Fix f;
    f.up();
    f.mqtt.hold(true);
    TEST_ASSERT_EQUAL_STRING("pausado", f.mqtt.status());
    TEST_ASSERT_FALSE(f.net.up);
    int c = f.net.connects;
    f.mqtt.tick(100);
    TEST_ASSERT_EQUAL(c, f.net.connects);
    f.mqtt.hold(false);
    f.mqtt.tick(101);                            // volta sem esperar
    TEST_ASSERT_EQUAL(c + 1, f.net.connects);

    WiFi._status = WL_DISCONNECTED;
    f.mqtt.tick(102);
    TEST_ASSERT_FALSE(f.net.up);
    int stops = f.net.stops;
    f.mqtt.tick(103);                            // já fechado: nada a fazer
    TEST_ASSERT_EQUAL(stops, f.net.stops);
    WiFi._status = WL_CONNECTED;

    // Configuração nova: reconecta já, mesmo durante a espera
    f.net.refuse = true;
    f.mqtt.tick(104);
    f.net.refuse = false;
    c = f.net.connects;
    f.mqtt.tick(105);
    TEST_ASSERT_EQUAL(c, f.net.connects);
    f.mqtt.reconnect();
    f.mqtt.tick(106);
    TEST_ASSERT_EQUAL(c + 1, f.net.connects);
}

// ── Discovery do Home Assistant ──────────────────────────────────────────────

static const char* DISCOVERY_TOPIC = "homeassistant/device/esp8266iot_00dead/config";
static const char* KEYS = "thdnbslcg";

// Último discovery publicado, já analisado
static bool discovery(const std::string& out, DynamicJsonDocument& doc, size_t nth = 0) {
    const Packet* p = published(out, DISCOVERY_TOPIC, nth);
    if (!p) return false;
    TEST_ASSERT_EQUAL_HEX8(0x01, p->flags);                   // retido
    std::string body = payloadOf(*p);
    TEST_ASSERT_FALSE(deserializeJson(doc, body.c_str()));     // JSON válido
    return true;
}

// Entidades ativas (com uniq_id), na ordem de KEYS
static std::string active(DynamicJsonDocument& doc) {
    std::string s;
    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL(9, (int)cmps.size());
    for (const char* k = KEYS; *k; k++) {
        char key[2] = {*k, 0};
        JsonObject e = cmps[key];
        TEST_ASSERT_FALSE(e.isNull());
        TEST_ASSERT_FALSE(e["p"].isNull());
        if (e.containsKey("uniq_id")) {
            TEST_ASSERT_EQUAL_STRING((std::string("esp8266iot_00dead_") + *k).c_str(), e["uniq_id"].as<const char*>());
            s += *k;
        } else {
            TEST_ASSERT_EQUAL(1, (int)e.size());               // só a plataforma: remove do HA
        }
    }
    return s;
}

static void configure(const char* sensor, const char* actuator) {
    strlcpy(cfg.iot.sensor, sensor, sizeof(cfg.iot.sensor));
    cfg.iot.sensorPin = sensor[0] ? 2 : -1;
    strlcpy(cfg.iot.actuator, actuator, sizeof(cfg.iot.actuator));
    cfg.iot.actuatorPin = actuator[0] ? 0 : -1;
}

void test_discovery_aparelho_e_topicos() {
    Fix f;
    configure("dht22", "rele");
    f.iot.begin();
    f.up();
    DynamicJsonDocument doc(4096);
    TEST_ASSERT_TRUE(discovery(f.net.out, doc));
    TEST_ASSERT_EQUAL_STRING("esp8266iot_00dead", doc["dev"]["ids"]);
    TEST_ASSERT_EQUAL_STRING("ESP8266 IoT", doc["dev"]["name"]);
    TEST_ASSERT_EQUAL_STRING(firmware_version, doc["dev"]["sw"]);
    TEST_ASSERT_EQUAL_STRING("esp8266-dns", doc["o"]["name"]);
    TEST_ASSERT_EQUAL_STRING("esp8266iot/00dead/state", doc["stat_t"]);
    TEST_ASSERT_EQUAL_STRING("esp8266iot/00dead/avail", doc["avty_t"]);
    TEST_ASSERT_EQUAL_STRING("esp8266iot/00dead/cmd", doc["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("ths", active(doc).c_str());

    JsonObject t = doc["cmps"]["t"];
    TEST_ASSERT_EQUAL_STRING("sensor", t["p"]);
    TEST_ASSERT_EQUAL_STRING("temperature", t["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("\xc2\xb0" "C", t["unit_of_meas"]);
    TEST_ASSERT_EQUAL_STRING("{{value_json.sensor.temperature|default(none)}}", t["val_tpl"]);
    TEST_ASSERT_EQUAL_STRING("humidity", doc["cmps"]["h"]["dev_cla"]);
    JsonObject s = doc["cmps"]["s"];
    TEST_ASSERT_EQUAL_STRING("switch", s["p"]);
    TEST_ASSERT_TRUE(s["name"].isNull() && s.containsKey("name"));
    TEST_ASSERT_EQUAL_STRING("ligar", s["pl_on"]);
    TEST_ASSERT_EQUAL_STRING("desligar", s["pl_off"]);

    // Ordem: disponibilidade, discovery, depois o estado
    auto v = packets(f.net.out);
    int avail = -1, disc = -1, state = -1;
    for (int i = 0; i < (int)v.size(); i++) {
        if (v[i].type != 3) continue;
        std::string tp = topicOf(v[i]);
        if (tp == "esp8266iot/00dead/avail") avail = i;
        if (tp == DISCOVERY_TOPIC) disc = i;
        if (tp == "esp8266iot/00dead/state") state = i;
    }
    TEST_ASSERT_TRUE(avail < disc && disc < state);
}

void test_discovery_por_tipo_de_modulo() {
    struct Case { const char* sensor; const char* actuator; const char* active; };
    const Case cases[] = {
        {"ds18b20", "", "t"}, {"distancia", "", "d"}, {"pulsos", "", "n"},
        {"pir", "led", "bl"}, {"contato", "fita", "bc"}, {"botao", "servo", "bg"},
        {"digital", "bomba", "bs"}, {"", "ventilador", "l"}, {"", "", ""},
    };
    for (const Case& c : cases) {
        setUp();
        Fix f;
        configure(c.sensor, c.actuator);
        f.iot.begin();
        f.up();
        DynamicJsonDocument doc(4096);
        TEST_ASSERT_TRUE(discovery(f.net.out, doc));
        TEST_ASSERT_EQUAL_STRING(c.active, active(doc).c_str());
    }
}

void test_discovery_detalhes_das_entidades() {
    Fix f;
    configure("pir", "led");
    f.iot.begin();
    f.up();
    DynamicJsonDocument doc(4096);
    TEST_ASSERT_TRUE(discovery(f.net.out, doc));
    JsonObject b = doc["cmps"]["b"];
    TEST_ASSERT_EQUAL_STRING("binary_sensor", b["p"]);
    TEST_ASSERT_EQUAL_STRING("motion", b["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("{{'ON' if value_json.sensor.active|default(0) else 'OFF'}}", b["val_tpl"]);
    JsonObject l = doc["cmps"]["l"];
    TEST_ASSERT_EQUAL_STRING("light", l["p"]);
    TEST_ASSERT_EQUAL_STRING("template", l["schema"]);
    TEST_ASSERT_EQUAL_STRING("desligar", l["cmd_off_tpl"]);
    TEST_ASSERT_TRUE(contains(l["cmd_on_tpl"].as<std::string>(), "nivel {{(brightness/2.55)|round|int}}"));
    TEST_ASSERT_EQUAL_STRING("{{(value_json.actuator.level*2.55)|round|int}}", l["bri_tpl"]);
    TEST_ASSERT_FALSE(l.containsKey("r_tpl"));

    setUp();
    Fix g;
    configure("contato", "fita");
    g.iot.begin();
    g.up();
    TEST_ASSERT_TRUE(discovery(g.net.out, doc));
    TEST_ASSERT_EQUAL_STRING("door", doc["cmps"]["b"]["dev_cla"]);
    JsonObject c = doc["cmps"]["c"];
    TEST_ASSERT_EQUAL_STRING("light", c["p"]);
    TEST_ASSERT_TRUE(contains(c["cmd_on_tpl"].as<std::string>(), "cor #{{'%02x%02x%02x'|format(red,green,blue)}}"));
    TEST_ASSERT_EQUAL_STRING("{{value_json.actuator.color[1:3]|int(base=16)}}", c["r_tpl"]);
    TEST_ASSERT_EQUAL_STRING("{{value_json.actuator.color[5:7]|int(base=16)}}", c["b_tpl"]);

    setUp();
    Fix h;
    configure("botao", "servo");
    h.iot.begin();
    h.up();
    TEST_ASSERT_TRUE(discovery(h.net.out, doc));
    TEST_ASSERT_FALSE(doc["cmps"]["b"].containsKey("dev_cla"));
    JsonObject g2 = doc["cmps"]["g"];
    TEST_ASSERT_EQUAL_STRING("number", g2["p"]);
    TEST_ASSERT_EQUAL(0, (int)g2["min"]);
    TEST_ASSERT_EQUAL(180, (int)g2["max"]);
    TEST_ASSERT_EQUAL_STRING("angulo {{value}}", g2["cmd_tpl"]);

    setUp();
    Fix k;
    configure("pulsos", "");
    k.iot.begin();
    k.up();
    TEST_ASSERT_TRUE(discovery(k.net.out, doc));
    TEST_ASSERT_EQUAL_STRING("Pulsos", doc["cmps"]["n"]["name"]);
}

// Trocar o módulo no painel republica: a entidade antiga sai, a nova entra
void test_discovery_republica_quando_o_modulo_muda() {
    Fix f;
    configure("", "rele");
    f.iot.begin();
    f.up();
    f.net.out.clear();
    f.mqtt.tick(10);
    TEST_ASSERT_NULL(published(f.net.out, DISCOVERY_TOPIC));

    configure("dht11", "led");
    f.iot.begin();
    f.mqtt.tick(11);
    DynamicJsonDocument doc(4096);
    TEST_ASSERT_TRUE(discovery(f.net.out, doc));
    TEST_ASSERT_EQUAL_STRING("thl", active(doc).c_str());
    TEST_ASSERT_EQUAL_STRING("switch", doc["cmps"]["s"]["p"]);       // marcada para remoção
    TEST_ASSERT_NOT_NULL(published(f.net.out, "esp8266iot/00dead/state"));

    f.net.out.clear();
    f.mqtt.tick(12);
    TEST_ASSERT_NULL(published(f.net.out, DISCOVERY_TOPIC));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_broker);
    RUN_TEST(test_desligado_sem_broker);
    RUN_TEST(test_connect_com_usuario_senha_e_testamento);
    RUN_TEST(test_connect_sem_usuario_ignora_senha);
    RUN_TEST(test_conectado_publica_disponibilidade_assina_e_publica_estado);
    RUN_TEST(test_broker_recusa);
    RUN_TEST(test_broker_sem_resposta);
    RUN_TEST(test_falhas_esperam_cada_vez_mais);
    RUN_TEST(test_broker_invalido);
    RUN_TEST(test_comando_aciona_e_publica_o_estado);
    RUN_TEST(test_comandos_que_nao_valem);
    RUN_TEST(test_pacotes_fora_de_hora_e_tamanho_longo);
    RUN_TEST(test_leitura_limitada_por_tick);
    RUN_TEST(test_estado_so_sai_quando_muda);
    RUN_TEST(test_estado_republicado_periodicamente);
    RUN_TEST(test_estado_longo_usa_tamanho_de_dois_bytes);
    RUN_TEST(test_ping_e_broker_parado);
    RUN_TEST(test_pausa_wifi_e_reconexao);
    RUN_TEST(test_discovery_aparelho_e_topicos);
    RUN_TEST(test_discovery_por_tipo_de_modulo);
    RUN_TEST(test_discovery_detalhes_das_entidades);
    RUN_TEST(test_discovery_republica_quando_o_modulo_muda);
    return UNITY_END();
}
