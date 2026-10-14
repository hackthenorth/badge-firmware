# Badge QA icon artwork — 2026-09-08

These replacements were generated with the built-in imagegen tool, saved as
96×96 transparent PNG masters, and converted through `regen-assets.py icons`.
The launcher draws white silhouettes behind existing dark icons; those source
images retain their original pixel art.

The Tic-Tac-Toe replacement has a light grid directly in its PNG and does not
use the extra launcher outline. Its empty cells and the holes in the O marks
have real alpha transparency. Icon regeneration rejects opaque PNG masters.

## Socials — `assets/icons/socials.png`

The PNG is a monochrome alpha stencil with transparent holes. The launcher
renders it in white using `icon_is_mask()`; there are no colored accents or
opaque black background pixels.

Prompt:

> A single bold BLACK @ symbol as a very clean monochrome stencil icon. Solid black glyph, no white paint anywhere in the symbol or the holes. Square transparent PNG canvas, real alpha channel: all empty space and all holes within the @ must be completely transparent. No checkerboard drawn into the image. This is a monochrome ALPHA MASK which will be tinted white in an application. Keep its shape simple and crisp, about 72% canvas occupancy, readable at 48x48. No texture, shading, gradients, noise, speckles, shadows, rough edges or extra objects.

## Sponsors — `assets/icons/sponsors.png`

Prompt:

> Create a single tiny pixel-art app icon: a little folded event map with three cream-white folded panels, a simple cyan path and one small red location pin. Simple crisp block pixels in the style of a 24x24 pixel retro badge launcher icon, centered in a square transparent canvas, about 80% canvas occupancy. Real transparent background. Bold white outer edging visible on a nearly black launcher background. No heart, no text, no gradient, no shadow. Must be readable at 48x48 pixels. Intended final source master is 96x96 PNG.

## Info & Rules — `assets/icons/rules.png`

Prompt:

> A single minimal retro pixel art information icon: a white circular outline containing a lowercase white i, precisely centered, nothing else. Transparent background. Crisp chunky pixel edges, no gradient, no shadow. Strongly readable at 48x48, matching tiny 24x24 game pixel art scaled to a 96x96 master, occupied circle diameter 72 of 96 pixels. Only the circle and lowercase i.

## Tic-Tac-Toe — `assets/icons/tictactoe.png`

Prompt:

> Generate a NEW clean transparent PNG icon for tic-tac-toe. A simple white 3x3 grid made of two vertical and two horizontal thin straight lines. Three colored marks only: solid red X in the top left cell, blue hollow O in the top right cell, blue hollow O in the center cell. Keep each mark clearly separated from the grid. Clean flat retro pixel art with solid colors and crisp straight block edges, no black outlines. Centered in square canvas with transparent margins. Real alpha transparency in empty cells and O holes and outside the grid. No background, no backdrop, no checkerboard, no texture, no shading, no speckles, no noise. It will be scaled to 48x48 pixels: use large simple shapes.
