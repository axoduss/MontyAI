# Controlla il file .onnx.json associato al modello
import json
config_path = "it_IT-riccardo-x_low.onnx.json"
with open(config_path) as f:
    config = json.load(f)
print("Sample rate:", config.get("audio", {}).get("sample_rate"))