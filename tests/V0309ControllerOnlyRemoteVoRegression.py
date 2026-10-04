from pathlib import Path

root = Path(__file__).resolve().parents[1]
plugin = (root / 'src/starfield/Plugin.cpp').read_text()
capture_h = (root / 'include/StarfieldDualSense/WwiseRemoteVoMirrorGate.h').read_text()
capture_cpp = (root / 'src/starfield/StarfieldAudioCapture.cpp').read_text()
policy_h = (root / 'include/StarfieldDualSense/RemoteVoOutputPolicy.h').read_text()
stop_h = (root / 'include/StarfieldDualSense/WwisePlayingId.h').read_text()
stop_cpp = (root / 'src/starfield/WwisePlayingId.cpp').read_text()
xmake = (root / 'xmake.lua').read_text()
config_toml = (root / 'config/StarfieldDualSense.toml').read_text()

assert 'originalPlayingId' in capture_h
assert '.originalPlayingId = record.returnedPlayingId' in capture_cpp
assert 'RemoteVoOriginalOutputAction' in policy_h
assert 'SpeakerOutputMode::ControllerOnly' in policy_h or 'SpeakerOutputMode::ControllerOnly' in plugin
assert 'stopWwisePlayingId' in stop_h and 'stopWwisePlayingId' in stop_cpp
assert 'REL::ID{ 150360 }' in stop_cpp or 'REL::ID kAkStopVoiceID{ 150360 }' in stop_cpp
assert '0x85, 0xD2, 0x74, 0x6C' in stop_cpp
assert 'decideRemoteVoOriginalOutput' in plugin
assert 'g_speakerManager->outputMode()' in plugin
assert 'originalPlayingId' in plugin
assert 'originalOutput=' in plugin
assert 'SpeakerOutputMode=Both' in plugin or 'outputMode=Both' in plugin or 'speakerOutputMode' in plugin
assert 'SpeakerOutputMode=ControllerOnly' in plugin or 'outputMode=ControllerOnly' in plugin or 'speakerOutputMode' in plugin
assert 'sds-remote-vo-output-policy-tests' in xmake
assert 'SpeakerOutputMode = "ControllerOnly"' in config_toml
assert 'ControllerOnly' in config_toml and 'controller playback is accepted' in config_toml

print('PASS controller-only remote VO regression')
