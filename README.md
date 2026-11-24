# Vokal Assistent

Et VST3-plugin til vokalspor i Cubase (Windows): klassisk EQ + de-esser, der kan
**analysere sporet** og **foreslå indstillinger** mod en mandlig eller kvindelig
målprofil. Brugerfladen er på engelsk.

## Installation

Hent den færdige plugin under **Actions → seneste grønne "Build" → Artifacts →
`Vokal-Assistent-Windows-VST3`** (eller under *Releases*, når der er lavet en).
Se [`docs/INSTALL.txt`](docs/INSTALL.txt).

## Sådan virker det

1. Tryk **Analyze** og afspil sangen (30–60 sekunder er nok).
2. Tryk **Stop and analyze**. Pluginnet analyserer den optagne lyd i baggrunden.
3. Læs rapporten, vælg profil (Auto/Male/Female) og mængde, og tryk
   **Apply suggestion**. Alt kan bagefter justeres manuelt og fortrydes.

Analysen gemmes i Cubase-projektet, så rapporten er der stadig næste gang.

### Hvad måles

| Mål | Metode | Bruges til |
|---|---|---|
| Grundtone og stemmeleje | YIN-pitchsporing på 4× nedsamplet signal | Mand/kvinde-gæt, lavcut-frekvens, målkurvens bund |
| Tonebalance | Langtidsspektrum (LTAS) af de tonale dele, 1/3-oktav (bredere ved høje stemmer) | EQ-forslag mod målprofilen |
| Resonanser | Hver harmonisk sammenlignes med sine naboer; faste toppe der går igen på tværs af melodien | Smalle cuts (kun ved tydelige og bekræftede toppe) |
| S-lyde | Rammer domineret af 4–14 kHz; S-spektrets top; simulation af de-esserens egen detektor | De-esser frekvens, threshold og range |
| Rumlen, støjgulv, clipping | Energi under stemmen, niveau i pauser, peak | Lavcut-stejlhed og noter i rapporten |

### EQ-forslaget

Målkurven bygger på langtidsspektre for tale (Byrne m.fl. 1994) gjort lidt
lysere i 2–5 kHz, som det er normalt for producerede vokaler, og bunden er
forankret i sangerens egen grundtone. Afvigelsen omsættes til højst 6 EQ-bånd:
lavcut, low shelf, op til 4 klokkebånd og high shelf. Tilpasningen er bevidst
forsigtig: afvigelser under 1 dB røres ikke, en kraftig grundtone accepteres,
boosts er begrænset til +6 dB, og bånd der kæmper mod hinanden straffes.

### De-esseren

Detektor: båndpas omkring S-frekvensen → envelope-follower → 4:1 med 6 dB blødt
knæ og et loft (*range*). Dæmpningen sker med et dynamisk filter, der er helt
neutralt når der ikke dæmpes: **Wide** (high shelf fra lidt under S-frekvensen)
eller **Focused** (klokke på S-frekvensen). **Listen** afspiller kun det, der
fjernes.

## Udvikling

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/va_analyze minvokal.wav --render minvokal-behandlet.wav   # analyse uden DAW
```

- `src/core/` – DSP og analyse uden JUCE (testbar overalt)
- `src/plugin/` – JUCE-plugin og brugerflade
- `tests/` – filter-, de-esser-, analyse- og closed-loop-tests med syntetiske stemmer
- GitHub Actions bygger Windows-VST3, kører alle tests og pluginval (strictness 10)
  på både Windows og Linux.

JUCE (AGPLv3 / JUCE-licens) og VST3 SDK hentes ved byg. Til personlig brug er
det fint; skal pluginnet en dag distribueres, skal licenserne ses efter.
