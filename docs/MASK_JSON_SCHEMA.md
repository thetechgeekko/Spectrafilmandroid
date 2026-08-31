# Spektrafilm mask-set JSON v1

> Stable interchange contract for `org.spektrafilm.mask-set`, version 1. The machine-readable
> canonical schema is [schema/spektrafilm-mask-v1.schema.json](schema/spektrafilm-mask-v1.schema.json).

A mask-set document stores ordered local adjustments. Each adjustment pairs a geometric/range mask
with a post-engine adjustment delta. Presets embed this document at their top-level `masks` property;
recipe envelopes embed the same preset document under `params`.

This format is Spektrafilm JSON interoperability, not Adobe Camera Raw/Lightroom XMP. Some concepts
map cleanly to Adobe masks, but Spektrafilm v1 applies luminance and color refinements mask-wide and
does not claim bit-identical Lightroom behavior. See [MASKING_SPEC.md](MASKING_SPEC.md) for that separate
XMP roadmap.

## Canonical document

Writers must emit the schema identifier, version, and adjustments array. This complete example has one
radial component, luminance and color refinements, and a non-zero exposure adjustment:

```json
{
  "schema": "org.spektrafilm.mask-set",
  "version": 1,
  "adjustments": [
    {
      "delta": {
        "exposureEv": 0.5,
        "temp": 0,
        "tint": 0,
        "saturation": 0,
        "contrast": 0,
        "hue": 0,
        "whites": 0,
        "blacks": 0,
        "clarity": 0,
        "sharpness": 0,
        "texture": 0,
        "highlights": 0,
        "shadows": 0
      },
      "mask": {
        "invert": false,
        "opacity": 0.75,
        "components": [
          {
            "mode": "ADD",
            "invert": false,
            "value": 1,
            "shape": {
              "type": "radial",
              "cx": 0.5,
              "cy": 0.5,
              "rx": 0.3,
              "ry": 0.2,
              "feather": 0.4,
              "angleDeg": 30
            }
          }
        ],
        "lumRange": {
          "min": 0.2,
          "max": 0.8,
          "feather": 0.1,
          "invert": false
        },
        "colorRange": {
          "r": 0.8,
          "g": 0.3,
          "b": 0.2,
          "tolerance": 0.25,
          "feather": 0.1,
          "invert": false
        }
      }
    }
  ]
}
```

Object property order is not significant. Adjustment and component array order is significant.

## Document fields

| Field | Type | Contract |
|---|---|---|
| `schema` | string | Must equal `org.spektrafilm.mask-set`. |
| `version` | integer | Must equal `1`. |
| `adjustments` | array | Ordered local adjustments; zero to 64 entries. |
| `adjustments[].delta` | object | Values applied where the final mask alpha is non-zero. |
| `adjustments[].mask` | object | Ordered components plus optional mask-wide range refinements. |

Canonical v1 writers must emit every field marked required by the JSON Schema and must not add
unversioned properties. A future wire change requires a new version and migration path.

## Mask composition

A mask has these canonical fields:

| Field | Type | Meaning |
|---|---|---|
| `components` | array | Zero to 32 components, folded in document order. |
| `invert` | boolean | Inverts the folded group alpha before group opacity. |
| `opacity` | number | Multiplies the group alpha. Producers normally use `0..1`. |
| `lumRange` | object, optional | Mask-wide output-luminance refinement. |
| `colorRange` | object, optional | Mask-wide output-color refinement. |

Each component contains `mode`, `invert`, `value`, and `shape`. Its shape coverage is inverted first
when `invert` is true, then multiplied by `value`, and finally folded into the accumulated alpha:

| `mode` | Fold operation, where `a` is accumulated alpha and `c` is component alpha |
|---|---|
| `ADD` | `a + c - a*c` |
| `SUBTRACT` | `a * (1-c)` |
| `INTERSECT` | `a * c` |

The accumulator starts at zero. After all components, group `invert` is applied and the result is
multiplied by `opacity`; runtime output alpha is clamped to `0..1`. An empty non-inverted mask selects
nothing. An empty inverted mask selects the whole image at the specified opacity.

## Shapes

Coordinates are resolution-independent image coordinates. Producers normally keep coordinates and
radii in `0..1`, use positive radial radii, and keep feather/value in `0..1`. The v1 decoder requires
values that remain finite after conversion to a 32-bit Float. The canonical schema therefore limits
every numeric field to `-3.4028235e38..3.4028235e38`, but it does not add narrower semantic bounds that
the implementation does not reject. Runtime behavior such as final-alpha and feather clamping still
applies.

### Linear

```json
{
  "type": "linear",
  "x0": 0.1,
  "y0": 0.2,
  "x1": 0.8,
  "y1": 0.9
}
```

Alpha ramps from zero to one along the vector from `(x0,y0)` to `(x1,y1)` using a clamped smoothstep.
A degenerate vector produces zero coverage.

### Radial

```json
{
  "type": "radial",
  "cx": 0.5,
  "cy": 0.5,
  "rx": 0.25,
  "ry": 0.25,
  "feather": 0.5,
  "angleDeg": 0
}
```

This is an ellipse centered at `(cx,cy)`, rotated by `angleDeg`. Feather controls the radial
falloff band. Runtime evaluation clamps feather to `0.001..1` and handles a near-zero radius without a
division by zero.

## Range refinements

`lumRange` contains `min`, `max`, `feather`, and `invert`. It applies a trapezoid gate to encoded output
luminance: full coverage between `min` and `max`, with a smooth falloff over `feather` on either side.

`colorRange` contains sampled encoded RGB values `r`, `g`, and `b`, plus `tolerance`, `feather`, and
`invert`. It compares color in a Rec.709-weighted chroma plane so luminance changes are handled
separately. Both refinements are mask-wide in schema v1.

## Adjustment delta

All 13 delta values are JSON numbers within the finite 32-bit Float range.

| Fields | Unit/meaning |
|---|---|
| `exposureEv` | Exposure in stops. |
| `hue` | Oklab hue rotation in degrees; the editor normally uses `-180..180`. |
| `temp`, `tint`, `saturation`, `contrast`, `whites`, `blacks` | Relative creative-control scales; the editor normally uses `-100..100`. |
| `clarity`, `sharpness`, `texture`, `highlights`, `shadows` | Spatial/local tonal scales; the editor normally uses `-100..100`. |

Zero is a no-op for every delta field.

## Reader compatibility and migration

The current reader accepts two top-level forms:

1. the v1 object documented here; and
2. the legacy v0 bare adjustments array written by earlier builds.

At the mask-codec boundary, a missing/null value decodes to an empty adjustment list. A preset that
omits its `masks` key leaves caller-owned state unchanged, so interoperable preset writers must emit the
key explicitly. A legacy array preserves the same adjustment order and meaning when wrapped as v1
during preset migration.

For defensive backward compatibility, the runtime decoder supplies defaults for some missing nested
fields and ignores many unknown nested properties. Interchange producers must not rely on that
leniency: the canonical JSON Schema requires the complete emitted shape and rejects extra properties.
An object with a wrong schema ID or unsupported version, an unknown blend mode, an unknown shape type,
a non-object adjustment/component, a known numeric value outside the finite Float range, more than 64
adjustments, or more than 32 components in one mask is rejected. A future version is never partially
applied.

When the mask document is embedded in a preset or recipe, the outer storage boundary also enforces a
4 MiB UTF-8 limit and general JSON limits of depth 32, 200,000 nodes, 4,096 array items, 2,048 object
keys, 65,536 characters per string/key, and 128 characters per scalar token. A lexical preflight
enforces these limits before the recursive object parser runs. The mask-specific 64/32 limits are
stricter where they overlap.

## Interoperability checklist

- Validate output with [the v1 JSON Schema](schema/spektrafilm-mask-v1.schema.json).
- Preserve array order and exact case for `ADD`, `SUBTRACT`, and `INTERSECT`.
- Emit ordinary JSON numbers inside `-3.4028235e38..3.4028235e38`; do not emit `NaN` or infinities.
- Use normalized image coordinates for portable geometry.
- Keep range refinements at mask level for v1.
- Increment the document version instead of silently changing field meaning.
