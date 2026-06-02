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
          { label: "Random (new each wake)", value: 0 },
          { label: "Sequential (m1 → m14, advance each wake)", value: 1 },
          { label: "m1 — Iron Shadow (jeep)", value: 2 },
          { label: "m2 — Blade Keeper (microbus)", value: 3 },
          { label: "m3 — atTractor (mash)", value: 4 },
          { label: "m4 — Oxidize Monk (retro)", value: 5 },
          { label: "m5 — Heavy Lady (dumper)", value: 6 },
          { label: "m6 — Spread Spot (baggi)", value: 7 },
          { label: "m7 — The Ripper (dragster)", value: 8 },
          { label: "m8 — Ancient Demon (hammer)", value: 9 },
          { label: "m9 — Arcan (oldcar)", value: 10 },
          { label: "m10 — Mad Surgeon (roadster)", value: 11 },
          { label: "m11 — Zippax (sedan)", value: 12 },
          { label: "m12 — Rivet Bier (track)", value: 13 },
          { label: "m13 — Piercator (universal)", value: 14 },
          { label: "m14 — Excorps (vagon)", value: 15 }
        ]
      }
    ]
  },
  {
    type: "section",
    items: [
      {
        type: "heading",
        defaultValue: "Idle rotation",
        size: 4
      },
      {
        type: "text",
        defaultValue: "Speed when you're not looking at the watch. While active (wrist raise or tap), the mech always spins at 4 fps."
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
