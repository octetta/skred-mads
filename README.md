# `skred`-MADS
**An Audio Engine for Hackers and Synthesists.**

`skred`-MADS is a minimalist digital signal processing (DSP) library built for
`miniaudio` users who want to see a tricked-out data source for synthesizers
and sample-playback.

> A MA low-level synth engine I've been working on is [here](https://github.com/octetta/skred/#readme). `skred`-MADS does not implement the full set of `skred`-ey things (yet).

## Core Philosophies

* **Universal Data Sources**: everything is a wavetable. Whether it’s a sine wave, a recorded kick drum, or a complex LFO shape, they are all governed by the same unified lookup logic.
* **`miniaudio`-native**: implements the `ma_data_source` interface, allowing you to drop it directly into a node graph.
* **Hacking is Fun**: let's get along.

## The Public Interface (incomplete... WIP)

| Function | Description |
| :--- | :--- |
| `skred_voice_init` | Initializes a voice with a buffer and binds it to the hardware sample rate. |
| `skred_voice_set_freq` | Sets target frequency with millisecond-accurate linear ramping. |
| `skred_voice_set_adsr` | Manages the Attack, Decay, Sustain, and Release state machine. |
| `skred_voice_set_lfo` | Assigns any wavetable as an LFO to modulate Freq, Vol, or Pan. |
| `skred_voice_trig` | Bypasses the ADSR for instantaneous one-shot triggering. |
| `...` | *look at `skred_ds.h` for more details* |

## Wiring into the `miniaudio` Graph

This data source is built for modularity. You don't just "play" a voice; you "wire" it.

### 1. Simple Node Attachment
Wrap your Skred voice in an `ma_data_source_node` so the graph can talk to it:
```c
ma_data_source_node_config cfg = ma_data_source_node_config_init(&my_skred_voice);
ma_data_source_node_init(p_graph, &cfg, NULL, &voice_node);

### 2. Creating a Bus (The Mixer Node)

To sum multiple voices into a single processing chain, use an ma_node
as a summing bus. By default, `miniaudio` nodes sum all attached inputs.

```c
// Create a generic node to act as a Mixer/Bus
ma_node_config mixer_cfg = ma_node_config_init();
mixer_cfg.vtable = &ma_splitter_node_vtable; // Standard passthrough/summing
ma_node_init(p_graph, &mixer_cfg, NULL, &mixer_bus);

// Attach voices to the mixer
ma_node_attach_output_bus(&voice_node_1, 0, &mixer_bus, 0);
ma_node_attach_output_bus(&voice_node_2, 0, &mixer_bus, 0);

// Attach the mixer to an effect
ma_node_attach_output_bus(&mixer_bus, 0, &filter_node, 0);
```

### 3. Dynamic Routing

The route command in the REPL demonstrates real-time graph manipulations.
By detaching and re-attaching the output bus of a voice node, you can move
signals through different effect paths without stopping the audio thread.

## Modular REPL Demo

The included `main.c` provides a real-time environment to test these connections.

### Example: The Pulsing Filter-Sweep

```bash
# on Linux... macOS and MSWindows coming soon...
make
./skred_demo
```

```
v 0           # Select voice 0
freq 440 0    # Set freq NOW
vol 0 0       # Set vol NOW
wave 1        # Set to Square wave
adsr 500 200 0.8 1000
on            # Trigger the note
route filter  # Pipe it through the resonant filter
cut 400       # Filter down
res 10        # Chane filter resonance
lfo_wave 1    # Use square wave for LFO
lfo 1 0 1 0   # Use LFO for amp at 1 Hz depth 1
route delay   # Switch the routing to the stereo delay
feed .5       # Change the delay feedback
```

> Yo! I heard you like hacks, so I hacked hacks into your hacks so you can hack.
