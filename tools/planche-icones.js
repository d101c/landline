// Planche des icônes de la barre : chaque état des deux styles, en sombre et
// en clair, à 16 px (taille réelle) et en grand, depuis lib/iconArt.js.
//
//   gjs -m tools/planche-icones.js DOSSIER
//   puis tools/planche-icones.sh DOSSIER pour les assembler en PNG.
//
// Les encres sont celles de stylesheet.css (les mêmes en clair et en sombre) ;
// le glyphe prend la couleur du texte de la barre.
import GLib from 'gi://GLib';

const out = ARGV[0];
const here = GLib.path_get_dirname(GLib.path_get_dirname(import.meta.url.replace('file://', '')));
const {ICON_STATES, ICON_STYLES, stateSvg} = await import(`file://${here}/extension/lib/iconArt.js`);
const INKS = {amber: '#f2b45c', green: '#6fd58e', red: '#ff8a7a'};
const SCHEMES = {
    sombre: [{glyph: '#ffffff', ...INKS}, '#1d1d20'],
    clair: [{glyph: '#1d1d20', ...INKS}, '#f6f5f4'],
};
GLib.mkdir_with_parents(out, 0o755);
for (const [scheme, [inks, bar]] of Object.entries(SCHEMES)) {
    for (const style of ICON_STYLES) {
        for (const state of ICON_STATES) {
            for (const glyph of [16, 96]) {
                // The glyph itself at `glyph` pixels, a margin of the bar around it.
                const size = glyph * 20 / 16;
                const svg = stateSvg(style, state, inks, {size, pad: 2, background: bar});
                const index = String(ICON_STATES.indexOf(state)).padStart(2, '0');
                GLib.file_set_contents(`${out}/${scheme}-${style}-${index}-${state}-${glyph}.svg`, svg);
            }
        }
    }
}
print(ICON_STATES.join(' '));
