#pragma once
#include <juce_osc/juce_osc.h>

// Lien OSC avec le studio virtuel (UE5) : ToneForge reste le moteur audio, l'autre côté n'est qu'une interface.
// Appli autonome seulement ; écoute 127.0.0.1:9000 (pas le réseau local), envoie vers 127.0.0.1:9001 (ou le port donné par /tf/hello). Tout tourne sur le fil des messages.
//  Entrée : /tf/hello [i port]   /tf/param s:id f:valeur (unités réelles, booléens 0/1)
//           /tf/load/amp|pedal|ir s:chemin   /tf/library   /tf/state
//  Sortie : /tf/levels f:entrée f:sortie (crêtes linéaires, 30 Hz)   /tf/param s:id f:valeur (à chaque changement)
//           /tf/loaded s:amp|pedal|ir s:chemin   /tf/error s:message
//           /tf/lib/begin i:n, n × /tf/lib/item i:index s:amp|pedal|ir s:marque s:dossier s:nom s:chemin, /tf/lib/end
//           (UDP : le client vérifie qu'il a reçu les n index, sinon redemande /tf/library)
class OscServer : private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>, private juce::Timer
{
public:
    static constexpr int inPort = 9000;

    explicit OscServer(ToneForgeProcessor& p) : proc(p)
    {
        for (auto* prm : proc.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm))
                params.push_back({ r, std::numeric_limits<float>::quiet_NaN() });
        if (socket.bindToPort(inPort, "127.0.0.1") && receiver.connectToSocket(socket))   // port déjà pris : reste sans OSC
        {
            receiver.addListener(this);
            connectOut(9001);
            startTimerHz(30);
        }
    }
    ~OscServer() override { stopTimer(); receiver.disconnect(); }

private:
    ToneForgeProcessor& proc;
    juce::DatagramSocket socket { false };   // avant receiver : doit lui survivre
    juce::OSCReceiver receiver;
    juce::OSCSender sender;
    bool out = false;
    struct Param { juce::RangedAudioParameter* p; float sent; };
    std::vector<Param> params;
    juce::File sentFile[3];
    bool resendFiles = true;

    void connectOut(int port) { out = sender.connect("127.0.0.1", port); }
    template <typename... A>
    void send(const juce::String& addr, A&&... a) { if (out) sender.send(addr, std::forward<A>(a)...); }
    static const char* kindName(int k) { return k == 0 ? "amp" : k == 1 ? "pedal" : "ir"; }
    juce::File file(int k) const { return proc.loadedFile(k); }

    static juce::String str(const juce::OSCMessage& m, int i) { return m.size() > i && m[i].isString() ? m[i].getString() : juce::String(); }
    static float num(const juce::OSCMessage& m, int i)
    {
        return m[i].isFloat32() ? m[i].getFloat32() : m[i].isInt32() ? (float) m[i].getInt32() : 0.f;
    }

    void oscMessageReceived(const juce::OSCMessage& m) override
    {
        const auto a = m.getAddressPattern().toString();
        if (a == "/tf/hello")
        {
            const int port = m.size() > 0 && (m[0].isInt32() || m[0].isFloat32()) ? (int) num(m, 0) : 9001;
            if (port < 1 || port > 65535)   // garde la connexion actuelle
                return send("/tf/error", "Port invalide : " + juce::String(port));
            connectOut(port);
            sendState();
        }
        else if (a == "/tf/param" && m.size() >= 2 && (m[1].isFloat32() || m[1].isInt32()))
        {
            if (auto* p = proc.apvts.getParameter(str(m, 0)))
                p->setValueNotifyingHost(p->convertTo0to1(num(m, 1)));
            else
                send("/tf/error", u8("Paramètre inconnu : ") + str(m, 0));
        }
        else if (a.startsWith("/tf/load/"))
        {
            const auto k = a.fromLastOccurrenceOf("/", false, false);
            const juce::File f(str(m, 0));
            const auto err = k == "ir" ? proc.loadIR(f) : k == "amp" || k == "pedal" ? proc.loadModel(f, k == "pedal")
                                                                                    : juce::String("Type inconnu : " + k);
            if (err.isNotEmpty())
                send("/tf/error", err);
        }
        else if (a == "/tf/library")
            sendLibrary();
        else if (a == "/tf/state")
            sendState();
    }

    void sendState()
    {
        for (auto& p : params)
            p.sent = std::numeric_limits<float>::quiet_NaN();   // le prochain tick renvoie tout
        resendFiles = true;
    }

    void sendLibrary()
    {
        struct Item { int kind; juce::File f; };
        std::vector<Item> items;
        const char* subs[] = { "models", "pedals", "irs" };
        for (int k = 0; k < 3; ++k)
            for (const auto& f : libraryRoot().getChildFile(subs[k]).findChildFiles(juce::File::findFiles, true, k == 2 ? "*.wav" : "*.nam"))
                items.push_back({ k, f });
        send("/tf/lib/begin", (juce::int32) items.size());
        juce::int32 idx = 0;
        for (const auto& it : items)
        {
            const auto grp = it.f.getParentDirectory().getFileName(), nm = it.f.getFileNameWithoutExtension();
            send("/tf/lib/item", idx++, juce::String(kindName(it.kind)), captureBrand(grp + " " + nm, it.kind == 1), grp, nm,
                 it.f.getFullPathName());
        }
        send("/tf/lib/end");
    }

    void timerCallback() override
    {
        send("/tf/levels", proc.oscInPeak.exchange(0.f), proc.oscOutPeak.exchange(0.f));
        for (auto& p : params)
            if (const float v = p.p->convertFrom0to1(p.p->getValue()); v != p.sent)
            {
                p.sent = v;
                send("/tf/param", p.p->getParameterID(), v);
            }
        for (int k = 0; k < 3; ++k)
            if (file(k) != sentFile[k] || resendFiles)
            {
                sentFile[k] = file(k);
                send("/tf/loaded", juce::String(kindName(k)), sentFile[k].getFullPathName());
            }
        resendFiles = false;
    }
};
