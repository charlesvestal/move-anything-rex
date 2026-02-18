/*
 * REX Player Module UI
 *
 * Uses shared sound generator UI base for file browsing.
 * Displays slice count and tempo info.
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/move-anything/shared/sound_generator_ui.mjs';

/* Create the UI */
const ui = createSoundGeneratorUI({
    moduleName: 'REX',
    showPolyphony: false,
    showOctave: false,

    /* Custom display: show slice count and tempo */
    drawCustom: (y, state) => {
        const sliceCount = host_module_get_param('slice_count') || '0';
        const tempo = host_module_get_param('tempo') || '0';

        if (parseInt(sliceCount) > 0) {
            print(2, y, sliceCount + ' slices', 1);
            y += 10;

            if (parseFloat(tempo) > 0) {
                print(2, y, tempo + ' BPM', 1);
                y += 10;
            }

            /* Show note range */
            const lastNote = 36 + parseInt(sliceCount) - 1;
            print(2, y, 'Notes: C2-' + noteToName(lastNote), 1);
            y += 10;
        }

        return y;
    }
});

/* Helper: MIDI note number to name */
function noteToName(note) {
    const names = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
    const octave = Math.floor(note / 12) - 2;
    return names[note % 12] + octave;
}

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
