module.exports = [
  {
    "type": "heading",
    "defaultValue": "Percival Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "color",
        "messageKey": "PrimaryColor",
        "label": "Color",
        "defaultValue": "0x000000"
      },
      {
        "type": "select",
        "messageKey": "Canvas",
        "label": "Canvas",
        "defaultValue": 0,
        "options": [
          {"label": "Paper", "value": 0},
          {"label": "Ink", "value": 1}
        ]
      },
      {
        "type": "select",
        "messageKey": "TempUnit",
        "label": "Temperature Unit",
        "defaultValue": 0,
        "options": [
          {"label": "Fahrenheit (°F)", "value": 0},
          {"label": "Celsius (°C)", "value": 1}
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
