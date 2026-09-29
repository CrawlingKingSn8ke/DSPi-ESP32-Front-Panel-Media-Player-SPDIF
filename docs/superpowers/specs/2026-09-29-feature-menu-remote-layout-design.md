# Feature, remote and menu-layout design

Date: 2026-09-29  
Base: `4a4be60` on `local/v1.2.1-stage-tube-limiter-basic`  
Target: a new isolated local UI stage, not a change to the published release

## Intent and boundaries

Make the front panel easier to read and navigate without changing DSPi sound processing, S/PDIF timing, BLE bonding, or the saved meaning of existing controls. Do not change the music player or its browser at all. Tube Modeller and Output Limiter already work in the current local build; this stage brings them into the established menu and Home-screen style. Users must be able to see that a settings, feature, remote, shortcut or preset list continues below the screen. Preserve the selected theme throughout. Produce a separately named OTA application and full USB image for testing; do not flash or publish them.

## Navigation

The top-level carousel has five entries, in this order: `Input`, `Music`, `Preset`, `Features`, `System`. `System` is always the last entry. `Features` opens a four-visible-row list in this order: `Loudness`, `Crossfeed`, `Leveller`, `Psy Bass`, `Sub Synth`, `Tube Modeller`, `Output Limiter`. Selecting a feature opens its settings list; Back returns to the feature list with its position retained. Existing feature toggles and parameter changes keep their DSPi commands and persistence behaviour.

`System` becomes a four-visible-row list: `Status`, `Screen Settings`, `Volume Limit`, `Wi-Fi Transfer/Update`, `Remote`. `Remote` is moved here from the top-level carousel. The current System and Screen Settings descendants retain their order and behaviour. The Music entry opens the existing music workflow without touching its screens, code or playback behaviour. Preset retains its current selection/save workflow. No music-player or playback action is added by navigation alone.

All feature-setting pages, System pages, Remote pages, and D-pad shortcut pages use the same `FontMedium` row labels, selection bar, semantic colours, and width-aware value column already used by Screen Settings. A page may have more than four entries, but only four full rows are visible at a time. Numeric edit screens retain the established large-number treatment. Labels and values may truncate with an ellipsis within their allocated columns; neither column may overdraw the other.

## Tube and limiter presentation

Tube `Drive` and `Mix` show the numeric part at the established large edit size, with a separate, smaller `dB` or `%` suffix. The suffixes use the same visual size and baseline. Crossing `-10 dB` must not shrink the number. In list rows, `Mix` uses `%` rather than `pct`; Drive uses `dB`. Reuse the existing percent/unit rendering path and its glyphs rather than introduce a new font style. Keep the exact DSPi value range, quantisation, and write timing from the base build. Output Limiter uses the same feature-list and settings-list typography but no new shortcut or full-screen notification.

## Remote under System

The Remote landing list shows the live connection state and saved device name, then `Key Map`, `D-pad Shortcuts`, `Find Remote` where applicable, and `Remove Remote` only when a saved profile exists. `Connected` appears as a status label; the actual device name is accent-coloured. While a paired device sleeps or reconnects, show `Reconnecting` and its saved name without claiming it is connected. With no profile, show `Not paired` and allow discovery. These are UI presentations of existing BLE state, not new bond logic.

`Key Map` is a scrolling list of all 21 existing mappable actions. Each row shows the action and its mapped remote button in the accent colour; an unmapped action shows dim `None`. Selecting a row opens a focused capture screen: first `Press button`, then `Release button` while held, then return to the list after a successful map. Back cancels without changing the old mapping. Existing mapping storage, conflict handling and BLE callbacks remain authoritative. `D-pad Shortcuts` becomes a scrolling list with the same row treatment. Selecting a shortcut opens a list of the available actions rather than the old single-choice carousel; Back without selection preserves its previous binding. Existing saved action IDs and bindings remain unchanged.

`Remove Remote` opens a confirmation that names the saved device. `Cancel` is the default, `Remove` is explicit, and Back cancels. Only the existing remove action clears the profile/bond. Disconnect, scanning failure or absent device must never be represented as a successful removal.

## Home status and feature feedback

The Home header has two rows: source and preset on the first, a centred row of active-feature symbols below it. Existing Home content and meter geometry stay below the current divider. Icon widths and gaps are measured before drawing; enabled Loudness, Crossfeed, Leveller, Psy Bass, Sub Synth, Tube and Limiter must fit together without overlap. Icons follow semantic main-text/accent/dim roles, not fixed cyan or white. Tube uses a compact vacuum-tube outline/filament motif based on the user's Mac Console reference; Limiter gets a distinct compact ceiling/shield motif. They remain identifiable at the screen's native 320×240 resolution.

Tube Toggle is appended to the D-pad shortcut choices without renumbering existing saved choices. Local Tube toggles and confirmed changes arriving from DSPi Console use the existing full-screen feature on/off notification path. Boot synchronisation and unchanged polling do not notify. Limiter has a Home status icon only: no full-screen on/off overlay and no D-pad/remote-map action. Its icon reflects a confirmed active/engaged status, not merely the presence of limiter settings in a preset. Readback on initial sync and after preset changes keeps the symbol current. No new polling work runs in the audio output task.

## Below-list indicator

On any vertically paged settings, feature, remote, shortcut or preset list, show one small downward chevron near the bottom-right only when at least one item lies below the last visible row. The indicator is absent on the final page or when all items fit; there is no up arrow and no arrow on carousel, edit or Music screens. Use a semantic accent colour and a simple fixed-size vector shape rather than a font glyph, so it works with every theme.

The chevron occupies a reserved right-corner footer area and must not collide with the Preset save hint or existing toasts. A toast covering that area temporarily suppresses it. The Music browser keeps its current scrollbar, partial-redraw logic and all other behaviour unchanged.

## Data flow and compatibility

Build one small list-layout helper that decides visible range, value-column spacing and whether more rows remain; renderers may still own their page-specific rows and footers. Keep BLE, Tube, Limiter and DSPi readback logic separate from this presentation helper. Append any new menu/action enum values rather than reassigning persisted shortcut IDs. Existing theme, preset, BLE profile, key map and D-pad preferences must load without migration loss. Retain support for the prior DSPi protocol level: unavailable Tube/Limiter capabilities should display as unavailable rather than emit unsupported writes or stale active icons.

Apply semantic colour roles everywhere touched: main text for labels, accent for selected/active values and symbols, dim text for inactive/`None`, and fixed fault/warning/clip colours. Do not alter selected user colours, VU geometry, analogue needle behaviour, or volume-meter response. Avoid introducing new blocking calls in the UI loop, media task, or Home polling. Existing audio-critical BLE scan suspension stays intact.

## Failure handling and verification

- A failed DSPi Tube/Limiter read leaves the last confirmed state or an explicit unavailable state; it does not generate a false notification.
- If BLE learning is cancelled, times out or disconnects, keep the previous map and return to a clear status/list view.
- Very long device names, feature values and preset names are measured and clipped within their own columns.
- Static contracts cover menu order, System-last placement, exact list membership, preserved shortcut IDs, Tube units, `%` display, arrow visibility boundaries, and icon packing.
- Focused renderer tests cover `-9.5` to `-10.0 dB`, short/long names, first/middle/final list page, toast suppression, all seven icons active, and mapped/unmapped Remote rows.
- Existing protocol, BLE and media regression tests pass. Build the ESP32-S3 application and full image, record changed files, binary sizes and SHA-256. Perform no flash, OTA upload, push or release update in this stage.

Hardware acceptance after the user elects to flash: check the affected menu pages for spacing and arrow behaviour; verify Tube and Limiter symbols with all features enabled; test Tube shortcut and Console state notifications; test Remote sleep/reconnect, key capture/cancel and removal confirmation. A basic music-playback smoke check confirms that unrelated playback still works, without changing the music player.
