module.exports = [
  {
    type: "heading",
    defaultValue: "Van Face"
  },
  {
    type: "text",
    defaultValue: "Rotating Vangers mechos on a turntable, with the time below. Settings persist on the watch."
  },
  {
    type: "section",
    items: [
      {
        type: "heading",
        defaultValue: "Vehicle",
        size: 4
      },
      {
        type: "select",
        messageKey: "Vehicle",
        defaultValue: 0,
        label: "Show",
        options: [
          { label: "Random (changes daily)", value: 0 },
          { label: "m1 — Iron Shadow (jeep)", value: 1 },
          { label: "m2 — Blade Keeper (microbus)", value: 2 },
          { label: "m3 — atTractor (mash)", value: 3 },
          { label: "m4 — Oxidize Monk (retro)", value: 4 },
          { label: "m5 — Heavy Lady (dumper)", value: 5 },
          { label: "m6 — Spread Spot (baggi)", value: 6 },
          { label: "m7 — The Ripper (dragster)", value: 7 },
          { label: "m8 — Ancient Demon (hammer)", value: 8 },
          { label: "m9 — Arcan (oldcar)", value: 9 },
          { label: "m10 — Mad Surgeon (roadster)", value: 10 },
          { label: "m11 — Zippax (sedan)", value: 11 },
          { label: "m12 — Rivet Bier (track)", value: 12 },
          { label: "m13 — Piercator (universal)", value: 13 },
          { label: "m14 — Excorps (vagon)", value: 14 }
        ]
      }
    ]
  },
  {
    type: "section",
    items: [
      {
        type: "heading",
        defaultValue: "Rotation",
        size: 4
      },
      {
        type: "select",
        messageKey: "FrameAdvanceSeconds",
        defaultValue: 5,
        label: "Advance every",
        options: [
          { label: "Off (no rotation)", value: 0 },
          { label: "1 second", value: 1 },
          { label: "3 seconds", value: 3 },
          { label: "5 seconds", value: 5 },
          { label: "10 seconds", value: 10 },
          { label: "30 seconds", value: 30 },
          { label: "1 minute", value: 60 }
        ]
      }
    ]
  },
  {
    type: "submit",
    defaultValue: "Save"
  }
];
