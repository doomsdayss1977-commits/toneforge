#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

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

// Requête dans son propre fil, annulable : le destructeur coupe la connexion et attend la fin du fil,
// pour qu'aucun code du plugin ne tourne encore après sa fermeture (déchargement du VST3).
class Job : private juce::Thread
{
public:
    using Done = std::function<void(const juce::String& reply, const juce::String& err)>;

    Job(Config c, juce::String sys, juce::String usr, Done d)
        : juce::Thread("ToneForge IA"), cfg(std::move(c)), system(std::move(sys)), user(std::move(usr)), done(std::move(d))
    {
        startThread();
    }

    ~Job() override
    {
        signalThreadShouldExit();
        {
            const juce::ScopedLock sl(lock);
            if (stream != nullptr)
                stream->cancel();
        }
        stopThread(10000);
    }

private:
    void run() override
    {
        juce::String err;
        const auto reply = chat(err);
        if (!threadShouldExit())
            juce::MessageManager::callAsync([d = done, reply, err] { d(reply, err); });
    }

    // Renvoie le texte de la réponse, ou vide et err rempli.
    juce::String chat(juce::String& err)
    {
        auto msg = [](const char* role, const juce::String& text)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("role", role);
            o->setProperty("content", text);
            return juce::var(o);
        };
        auto* req = new juce::DynamicObject();
        req->setProperty("model", cfg.model);
        req->setProperty("messages", juce::Array<juce::var> { msg("system", system), msg("user", user) });
        req->setProperty("temperature", 0.4);
        req->setProperty("max_tokens", 4000);

        juce::WebInputStream s(juce::URL("https://openrouter.ai/api/v1/chat/completions")
                                   .withPOSTData(juce::JSON::toString(juce::var(req), true)),
                               true);
        s.withExtraHeaders("Content-Type: application/json\r\nAuthorization: Bearer " + cfg.key + "\r\nX-Title: ToneForge")
            .withConnectionTimeout(120000);
        {
            const juce::ScopedLock sl(lock);
            if (threadShouldExit())
                return {};
            stream = &s;
        }
        const bool ok = s.connect(nullptr);
        const auto body = ok ? s.readEntireStreamAsString() : juce::String();
        {
            const juce::ScopedLock sl(lock);
            stream = nullptr;
        }
        if (!ok)
        {
            err = juce::String(juce::CharPointer_UTF8("Pas de connexion à OpenRouter."));
            return {};
        }
        const auto r = juce::JSON::parse(body);
        if (s.getStatusCode() != 200)
        {
            err = "OpenRouter " + juce::String(s.getStatusCode()) + " : " + r["error"]["message"].toString();
            return {};
        }
        return r["choices"][0]["message"]["content"].toString();
    }

    const Config cfg;
    const juce::String system, user;
    const Done done;
    juce::CriticalSection lock;
    juce::WebInputStream* stream = nullptr;
};
} // namespace ai
