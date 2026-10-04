#include <StarfieldDualSense/FireMarkerBridge.h>
#include <StarfieldDualSense/GameWorldGate.h>

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace
{
    int failures = 0;
    void check(bool ok, const char* label)
    {
        if (!ok) { std::cerr << "FAIL " << label << '\n'; ++failures; }
    }
}

int main()
{
    sds::GameWorldGate gate;
    check(!gate.available(), "world access starts suspended until the UI snapshot");
    for (bool mainMenu : {false, true}) {
        for (bool loading : {false, true}) {
            gate.snapshot(mainMenu, loading);
            check(gate.available() == (!mainMenu && !loading), "initial menu snapshot");
            check(gate.mainMenuOpen() == mainMenu, "UI input retains the specific MainMenu state");
        }
    }
    for (bool closeMainFirst : {false, true}) {
        gate.snapshot(false, false);
        gate.observeMenu("MainMenu", true);
        gate.observeMenu("LoadingMenu", true);
        gate.observeMenu(closeMainFirst ? "MainMenu" : "LoadingMenu", false);
        check(!gate.available(), "closing one world blocker cannot release the other");
        gate.observeMenu(closeMainFirst ? "LoadingMenu" : "MainMenu", false);
        check(gate.available(), "world resumes after both blockers close");
    }
    gate.observeMenu("FaderMenu", true);
    check(gate.available(), "a cinematic fade alone is not world teardown");

    auto player = std::make_shared<RE::PlayerCharacter>();
    RE::PlayerCharacter::instance = player.get();
    player->manager = std::make_shared<RE::BSAnimationGraphManager>();
    auto graph = std::make_shared<RE::BSAnimationGraph>();
    player->manager->graphs.push_back(graph);
    RE::TESObjectWEAP weapon;
    RE::ActorItemEquipped::Event equip{&weapon, player};
    sds::FireMarkerBridge bridge([](sds::GameEvent) { return true; }, [&] { return gate.available(); });
    check(bridge.start(), "bridge starts with the global equip sink");

    gate.snapshot(true, false);
    (void)bridge.ProcessEvent(equip, nullptr);
    check(RE::PlayerCharacter::worldReads == 0 && graph->sinks.empty(), "MainMenu cannot query or arm player graphs");
    gate.observeMenu("MainMenu", false);
    (void)bridge.ProcessEvent(equip, nullptr);
    check(graph->sinks.size() == 1, "fresh gameplay equip arms the current graph");

    gate.observeMenu("LoadingMenu", true);
    bridge.suspendPlayerGraphsForWorldTeardown();
    check(graph->sinks.empty(), "loading detaches the old graph sink");
    std::weak_ptr<RE::BSAnimationGraph> oldGraph = graph;
    player->manager.reset();
    graph.reset();
    check(oldGraph.expired(), "bridge releases strong references before world objects are destroyed");
    const auto readsBefore = RE::PlayerCharacter::worldReads;
    RE::ActorItemEquipped::Event stale{reinterpret_cast<RE::TESForm*>(1), player};
    (void)bridge.ProcessEvent(stale, nullptr);
    check(RE::PlayerCharacter::worldReads == readsBefore, "late equip during loading cannot touch the player or stale item");
    gate.observeMenu("MainMenu", true);
    gate.observeMenu("LoadingMenu", false);
    (void)bridge.ProcessEvent(stale, nullptr);
    check(RE::PlayerCharacter::worldReads == readsBefore, "return-to-menu remains blocked after loading closes");

    player->manager = std::make_shared<RE::BSAnimationGraphManager>();
    graph = std::make_shared<RE::BSAnimationGraph>();
    player->manager->graphs.push_back(graph);
    gate.observeMenu("MainMenu", false);
    (void)bridge.ProcessEvent(equip, nullptr);
    check(graph->sinks.size() == 1, "fresh bootstrap reattaches only the new world's graph");

    // Teardown waits for an already-entered equip handler, then detaches its result.
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, resume = false;
    player->beforeGraphRead = [&] {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        if (!cv.wait_for(lock, 5s, [&] { return resume; })) std::abort();
    };
    std::thread arming([&] { (void)bridge.ProcessEvent(equip, nullptr); });
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 5s, [&] { return entered; })) std::abort();
    }
    gate.observeMenu("LoadingMenu", true);
    std::thread teardown([&] { bridge.suspendPlayerGraphsForWorldTeardown(); });
    { std::scoped_lock lock(mutex); resume = true; }
    cv.notify_all();
    arming.join();
    teardown.join();
    player->beforeGraphRead = {};
    check(graph->sinks.empty(), "an in-flight equip handler cannot rearm after teardown completes");

    bridge.stop();
    check(RE::ActorItemEquipped::Event::GetEventSource()->sinks.empty(), "shutdown removes the global equip sink");
    gate.suspend();
    check(!gate.available(), "shutdown suspends world access");
    std::cout << "World lifecycle: " << failures << " failure(s)\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
