#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace RE
{
    enum class BSEventNotifyControl { kContinue };
    enum class FormType { kWEAP };
    template<class T> class BSTEventSource;
    template<class T> class BSTEventSink
    {
    public:
        virtual ~BSTEventSink() = default;
        virtual BSEventNotifyControl ProcessEvent(const T&, BSTEventSource<T>*) = 0;
    };
    template<class T> class BSTEventSource
    {
    public:
        void RegisterSink(BSTEventSink<T>* sink) { sinks.push_back(sink); }
        void UnregisterSink(BSTEventSink<T>* sink) { std::erase(sinks, sink); }
        std::vector<BSTEventSink<T>*> sinks;
    };
    template<class T> using BSTSmartPointer = std::shared_ptr<T>;
    template<class T> using NiPointer = std::shared_ptr<T>;
    struct BSAnimationGraphEvent { std::uintptr_t words[3]{}; };
    class BSAnimationGraph : public BSTEventSource<BSAnimationGraphEvent> {};
    struct BSAnimationGraphManager { std::vector<BSTSmartPointer<BSAnimationGraph>> graphs; };
    class TESForm
    {
    public:
        FormType GetFormType() const { return FormType::kWEAP; }
        const char* GetFormEditorID() const { return "Maelstrom"; }
    };
    class TESObjectWEAP : public TESForm
    {
    public:
        const char* GetFullName() const { return "Maelstrom"; }
    };
    class Actor {};
    class PlayerCharacter : public Actor
    {
    public:
        static PlayerCharacter* GetSingleton() { ++worldReads; return instance; }
        bool GetAnimationGraphManagerImpl(BSTSmartPointer<BSAnimationGraphManager>& out)
        {
            if (beforeGraphRead) beforeGraphRead();
            out = manager;
            return bool(out);
        }
        BSTSmartPointer<BSAnimationGraphManager> manager;
        std::function<void()> beforeGraphRead;
        static inline PlayerCharacter* instance{};
        static inline int worldReads{};
    };
    struct ActorItemEquipped
    {
        struct Event
        {
            TESForm* item{};
            NiPointer<Actor> actor{};
            static BSTEventSource<Event>* GetEventSource()
            {
                static BSTEventSource<Event> source;
                return &source;
            }
        };
    };
}
