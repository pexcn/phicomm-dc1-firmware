#include <cassert>
#include <cstdio>
#include <string>
#include "PubSubClient.h"

struct Debug { static void AddInfo(const char *, ...) {} };
struct Config { static bool prepareRestart() { return true; } };
struct Esp { void reset() { assert(false); } } ESP;
struct Led { static void led(unsigned) {} };
static unsigned callbacks;
static std::string expectedTopic, expectedPayload;
static PubSubClient *mqtt;
struct Module {
    void mqttCallback(char *topic, char *payload, char *command)
    {
        ++callbacks;
        assert(topic == expectedTopic && payload == expectedPayload);
        assert(strcmp(command, "power1") == 0);
        // Real command handlers may publish from inside this callback.
        assert(mqtt->publish("reply", "ok"));
    }
};
static Module device, *module = &device;
struct Framework { static void callback(char *, byte *, unsigned); };
#include "mqtt-callback.inc"

static std::vector<uint8_t> packet(bool qos1, const std::string &payload)
{
    const auto &topic = expectedTopic;
    std::vector<uint8_t> body = {uint8_t(topic.size() >> 8), uint8_t(topic.size())};
    body.insert(body.end(), topic.begin(), topic.end());
    if (qos1) body.insert(body.end(), {0x12, 0x34});
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<uint8_t> data = {uint8_t(qos1 ? 0x32 : 0x30)};
    size_t remaining = body.size();
    do {
        uint8_t digit = remaining % 128;
        remaining /= 128;
        data.push_back(digit | (remaining ? 0x80 : 0));
    } while (remaining);
    data.insert(data.end(), body.begin(), body.end());
    return data;
}

int main()
{
    expectedTopic = "dc1/cmnd/power1";
    for (bool qos1 : {false, true})
    {
        Client transport;
        PubSubClient client("broker", 1883, Framework::callback, transport);
        mqtt = &client;
        assert(client.getBufferSize() == 768);
        transport.incoming = {0x20, 0x02, 0x00, 0x00};
        assert(client.connect("test"));
        const size_t fullPayload = 768 - 5 - expectedTopic.size() - (qos1 ? 2 : 0);
        for (const auto &payload : {std::string(), std::string("on"), std::string("off"),
                std::string(fullPayload - 1, 'x'), std::string(fullPayload, 'y'),
                std::string(fullPayload + 1, 'z')})
        {
            expectedPayload = payload;
            const auto data = packet(qos1, payload);
            transport.incoming.insert(transport.incoming.end(), data.begin(), data.end());
            transport.outgoing.clear();
            callbacks = 0;
            assert(client.loop());
            assert(callbacks == (data.size() <= 768 ? 1U : 0U));
            if (qos1 && callbacks)
            {
                const auto &sent = transport.outgoing;
                assert(sent.size() >= 4);
                assert(std::vector<uint8_t>(sent.end() - 4, sent.end())
                    == std::vector<uint8_t>({0x40, 0x02, 0x12, 0x34}));
            }
        }
    }
    puts("PASS: real PubSubClient 2.8 callback: empty/full/oversized QoS 0/1 packets, in-callback publish and PUBACK");
}
