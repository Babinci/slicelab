# Third-party notices

SliceLab uses Audio Plugin Coder by Noizefield as a pinned source submodule. APC is MIT licensed. Its license is `vendor/audio-plugin-coder/LICENSE.md`.

JUCE 9.0.1 is the framework's pinned nested dependency. This open-source project uses JUCE under its AGPLv3 option. JUCE's exact license and notices are in `vendor/audio-plugin-coder/_tools/JUCE/LICENSE.md`.

Rubber Band Library is linked from the Linux host. Its free-software license is GPL version 2 or later. Its source and terms are available from [Rubber Band Library](https://breakfastquay.com/rubberband/).

The optional remote stem worker uses ZFTurbo's four-source BS-RoFormer checkpoint `model_bs_roformer_ep_17_sdr_9.6568.ckpt`. Credit for the checkpoint goes to ZFTurbo. Its [specific MIT grant and training-data qualification](https://github.com/ZFTurbo/Music-Source-Separation-Training/issues/254) are separate from the inference software's license. Weights are downloaded to a private cache and are not included in this source repository or VST3 bundle. The pinned model and configuration hashes are recorded by the worker.

Serato Sample is the user's workflow reference. SliceLab is an independent implementation and is not affiliated with or endorsed by Serato. No Serato code, model, branding, or artwork is included.
