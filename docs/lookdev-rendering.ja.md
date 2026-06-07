# LookDev Rendering Notes

English version: [lookdev-rendering.md](lookdev-rendering.md)

このドキュメントでは、v1.2 時点の描画確認向け LookDev 機能と、それを確認するための UI をまとめます。

## Main View

![ChatLookDev main viewport](../images/screenshot.png)

メイン viewport では、glTF/GLB の PBR model、HDR IBL、Sun lighting、docking ImGui layout、AI Chat panel を同時に扱います。Lighting panel には HDRI、Sun、shadow、view、sky color の操作を集約しています。

## Display Modes

![Normal display mode](../images/image3.png)

Lighting panel の Display Mode combo で、PBR shader の出力を Beauty と debug view の間で切り替えられます。Normal view は tangent-space normal map 適用後の最終 shading normal を可視化するため、tangent や normal map の問題を確認しやすくなります。

## Sun Shadows

![Sponza with Sun shadow](../images/image2.png)

v1.2 では単一の orthographic Sun shadow map を追加しています。shadow map は model transform 後の scene bounds に fit し、direct Sun lighting にだけ適用します。IBL、emissive、sky rendering には shadow を掛けません。

Shadows section では次を操作できます。

- Enable Shadows
- Shadow Resolution: `1024`、`2048`、`4096`
- Shadow Strength
- Shadow Bias
- Shadow Softness
- Shadow Fit Scale
- Shadow Mask View / Beauty View quick toggle

Alpha Mask material は shadow depth pixel shader で cutout shadow を落とします。Blend material は v1.2 では shadow caster から除外しています。

## Sun Direction UI

Sun section には vector と angle の両方の操作を用意しています。

- `Direction`: 直接の XYZ light-ray vector。
- `Azimuth deg` / `Elevation deg`: 人間が理解しやすい sun-source direction。
- `Noon`、`Side`、`Rim`、`Low`: 簡易プリセット。

Viewport 右上には小さな Sun direction overlay も表示します。camera-relative な表示なので、scene 内に大きな helper object を置かずに、太陽がどちらから来ているかを確認できます。

## AI Actions

chat action system は、ImGui control と同じ render state を変更できます。view settings、Sun settings、shadow settings、material parameters、camera、model transform が対象です。実際に renderer へ反映する前に、UI 側で各 action を validation します。
