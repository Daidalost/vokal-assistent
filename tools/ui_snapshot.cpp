// Developer tool: runs the plugin headless on a WAV file (analyse + apply) and
// writes a PNG of the editor. Used to check the user interface without a DAW.
//   va_ui_snapshot input.wav output.png
#include "../src/plugin/PluginProcessor.h"
#include "WavIO.h"

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: va_ui_snapshot input.wav output.png [--no-analysis]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI init;
    va::Audio audio;
    std::string err;
    if (! va::readWav (argv[1], audio, err)) { std::printf ("%s\n", err.c_str()); return 1; }
    const bool analyse = ! (argc > 3 && juce::String (argv[3]) == "--no-analysis");

    auto proc = std::make_unique<VokalAssistentProcessor>();
    const int block = 512;
    proc->setPlayConfigDetails (2, 2, audio.sampleRate, block);
    proc->prepareToPlay (audio.sampleRate, block);
    const auto mono = audio.mono();
    juce::MidiBuffer midi;
    auto run = [&] (size_t from, size_t to)
    {
        juce::AudioBuffer<float> buf (2, block);
        for (size_t pos = from; pos + block <= to; pos += block)
        {
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, mono.data() + pos, block);
            proc->processBlock (buf, midi);
        }
    };
    if (analyse)
    {
        proc->startAnalysis();
        run (0, mono.size());
        proc->stopAnalysis();
        for (int i = 0; i < 100 && proc->getAnalysisState() != VokalAssistentProcessor::AnalysisState::Idle; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        proc->applySuggestion();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setVisible (true);
    for (int i = 0; i < 20; ++i)
    {
        const size_t start = (size_t) (i * 0.1 * audio.sampleRate) % (mono.size() - 8192);
        run (start, start + (size_t) (0.1 * audio.sampleRate));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
    }
    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    editor.reset();
    proc.reset();
    return 0;
}
