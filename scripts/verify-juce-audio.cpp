#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>

// Small metadata/PCM probe linked against the same JUCE build as SliceLab.
// Arguments: source file, optional raw interleaved float32 output file.
int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
        return 2;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])));
    if (! reader || reader->lengthInSamples <= 0 || reader->lengthInSamples > 10000000 || reader->numChannels < 1 || reader->numChannels > 2)
        return 3;

    juce::AudioBuffer<float> audio(2, static_cast<int>(reader->lengthInSamples));
    const bool decoded = reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    std::cout << "sample_rate=" << reader->sampleRate << " channels=" << reader->numChannels
              << " frames=" << reader->lengthInSamples << " decoded=" << decoded << '\n';
    if (! decoded)
        return 4;
    if (argc == 3)
    {
        std::ofstream out(argv[2], std::ios::binary);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (unsigned c = 0; c < reader->numChannels; ++c)
            {
                const float sample = audio.getSample(static_cast<int>(c), i);
                out.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
            }
        if (! out)
            return 5;
    }
    return 0;
}
