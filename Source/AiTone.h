#pragma once
#include <juce_core/juce_core.h>

// Appel OpenRouter pour le mode « Son IA ».
namespace ai
{
struct Config
{
    juce::String key, model { "anthropic/claude-sonnet-5" };
};

// Clé : variable OPENROUTER_API_KEY, sinon celle du Maître de Donjon IA (lue sur place, jamais recopiée).
inline Config config()
{
    Config c;
    c.key = juce::SystemStats::getEnvironmentVariable("OPENROUTER_API_KEY", {}).trim();
    const auto o = juce::JSON::parse(juce::File("C:\\dev\\dnd_dm\\data\\settings.json"))["openrouter"];
    if (c.key.isEmpty())
        c.key = o["api_key"].toString().trim();
    if (o["model"].toString().isNotEmpty())
        c.model = o["model"].toString();
    return c;
}

// Bloquant : à appeler hors du thread de l'interface. Renvoie le texte de la réponse, ou vide et err rempli.
inline juce::String chat(const Config& c, const juce::String& system, const juce::String& user, juce::String& err)
{
    auto msg = [](const char* role, const juce::String& text)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("role", role);
        o->setProperty("content", text);
        return juce::var(o);
    };
    auto* req = new juce::DynamicObject();
    req->setProperty("model", c.model);
    req->setProperty("messages", juce::Array<juce::var> { msg("system", system), msg("user", user) });
    req->setProperty("temperature", 0.4);
    req->setProperty("max_tokens", 2000);

    int status = 0;
    const auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                          .withExtraHeaders("Content-Type: application/json\r\nAuthorization: Bearer " + c.key
                                            + "\r\nX-Title: ToneForge")
                          .withConnectionTimeoutMs(120000)
                          .withStatusCode(&status);
    auto stream = juce::URL("https://openrouter.ai/api/v1/chat/completions")
                      .withPOSTData(juce::JSON::toString(juce::var(req), true))
                      .createInputStream(opts);
    if (stream == nullptr)
    {
        err = juce::String(juce::CharPointer_UTF8("Pas de connexion à OpenRouter."));
        return {};
    }
    const auto r = juce::JSON::parse(stream->readEntireStreamAsString());
    if (status != 200)
    {
        err = "OpenRouter " + juce::String(status) + " : " + r["error"]["message"].toString();
        return {};
    }
    return r["choices"][0]["message"]["content"].toString();
}
} // namespace ai
