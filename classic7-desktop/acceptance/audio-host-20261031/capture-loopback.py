import sys, json, datetime, pathlib, wave
sys.path.insert(0, r'K:\CodexResources\2026-10-31\classic7-audio-py37')
import soundcard as sc
import numpy as np

out = pathlib.Path(__file__).parent
label = sys.argv[1]
speaker = sc.default_speaker()
loop = sc.get_microphone(speaker.id, include_loopback=True)
start = datetime.datetime.now(datetime.timezone.utc).isoformat()
data = loop.record(numframes=48000 * 6, samplerate=48000, channels=2)
result = dict(label=label, start_utc=start, end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              endpoint=speaker.name, loopback=loop.isloopback, samplerate=48000,
              frames=len(data), peak=float(np.max(np.abs(data))),
              rms=float(np.sqrt(np.mean(data ** 2))),
              nonzero_samples=int(np.count_nonzero(data)))
with wave.open(str(out / (label + '.wav')), 'wb') as wav:
    wav.setnchannels(2)
    wav.setsampwidth(2)
    wav.setframerate(48000)
    wav.writeframes((np.clip(data, -1, 1) * 32767).astype('<i2').tobytes())
(out / (label + '.json')).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf8')
print(json.dumps(result, ensure_ascii=False))
