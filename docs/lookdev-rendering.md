# LookDev Rendering Notes

Japanese version: [lookdev-rendering.ja.md](lookdev-rendering.ja.md)

This document summarizes the v1.2 rendering-facing LookDev features and the UI surfaces used to inspect them.

## Main View

![ChatLookDev main viewport](../images/screenshot.png)

The main viewport combines a glTF/GLB PBR model, HDR IBL, Sun lighting, a docked ImGui layout, and the AI Chat panel. The Lighting panel exposes HDRI, Sun, shadow, view, and sky controls in one place.

## Display Modes

![Normal display mode](../images/image3.png)

The display mode combo in the Lighting panel switches the PBR shader output between Beauty and debug views. The Normal view visualizes the final shading normal after tangent-space normal map application, which makes tangent and normal-map issues easier to inspect.

## Sun Shadows

![Sponza with Sun shadow](../images/image2.png)

v1.2 adds a single orthographic Sun shadow map. It is fitted to the transformed scene bounds and applied only to direct Sun lighting. IBL, emissive, and sky rendering are not shadowed.

The Shadows section controls:

- Enable Shadows
- Shadow Resolution: `1024`, `2048`, or `4096`
- Shadow Strength
- Shadow Bias
- Shadow Softness
- Shadow Fit Scale
- Shadow Mask View / Beauty View quick toggle

Alpha Mask materials cast cutout shadows through the shadow depth pixel shader. Blend materials are excluded from shadow casting in v1.2.

## Sun Direction UI

The Sun section provides both vector and angle controls:

- `Direction`: direct XYZ light-ray vector.
- `Azimuth deg` / `Elevation deg`: human-readable sun-source direction.
- `Noon`, `Side`, `Rim`, and `Low`: quick presets.

The viewport also draws a small Sun direction overlay. It is camera-relative, so the user can see where the Sun is coming from without placing a large helper object in the scene.

## AI Actions

The chat action system can modify the same render state that the ImGui controls edit, including view settings, Sun settings, shadow settings, material parameters, camera, and model transform. The UI still validates every action before committing changes to the renderer.
