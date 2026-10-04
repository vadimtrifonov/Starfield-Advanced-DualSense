from pathlib import Path

root = Path(__file__).resolve().parents[1]

haptics_engine = (root / "src/core/HapticsEngine.cpp").read_text(encoding="utf-8")
haptics_manager = (root / "src/core/HapticsManager.cpp").read_text(encoding="utf-8")
effects_engine = (root / "src/core/EffectsEngine.cpp").read_text(encoding="utf-8")
plugin = (root / "src/starfield/Plugin.cpp").read_text(encoding="utf-8")
feedback_gate = (root / "include/StarfieldDualSense/GameplayFeedbackGate.h").read_text(encoding="utf-8")

checks = [
    (
        "runtime remains v0.6.3",
        'constexpr std::string_view kVersion = "0.6.3";' in plugin,
    ),
    (
        "shared feedback policy includes GalaxyStarMapMenu",
        '"GalaxyStarMapMenu"' in feedback_gate,
    ),
    (
        "general haptics use shared menu suspension",
        "_feedbackGate.handle(event)" in haptics_engine
        and "_feedbackGate.blocked()" in haptics_engine,
    ),
    (
        "ship haptics use shared gameplay and transition suspension",
        "_engine.gameplayBlocked()" in haptics_manager
        and "_engine.shipTransitionBlocked()" in haptics_manager,
    ),
    (
        "ship map-open path clears live propulsion",
        "_shipPropulsionContinuous = {};" in haptics_manager,
    ),
    (
        "ship map-open path clears sustained laser texture",
        "clearShipLaserLocked();" in haptics_manager,
    ),
    (
        "land-vehicle map-open path clears continuous motion",
        "_landVehicleMotion = {};" in haptics_manager
        and "_landVehicleContinuous = {};" in haptics_manager
        and "_landVehicleFeel.reset();" in haptics_manager,
    ),
    (
        "ship adaptive-trigger path uses shared menu suspension",
        "_feedbackGate.handle(event)" in effects_engine
        and "_feedbackGate.blocked()" in effects_engine,
    ),
    (
        "ship map-open path clears adaptive triggers",
        "_state.output.leftTrigger = {};" in effects_engine
        and "_state.output.rightTrigger = {};" in effects_engine,
    ),
]

failed = False

for label, ok in checks:
    if ok:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}")
        failed = True

if failed:
    raise SystemExit(1)

print()
print("PASS v0.6.3 GalaxyStarMapMenu haptics/trigger blocking contract")
