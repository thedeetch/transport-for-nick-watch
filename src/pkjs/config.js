var Clay = require('pebble-clay');
var clayConfig = [
    {
        "type": "heading",
        "defaultValue": "TfL Settings"
    },
    {
        "type": "section",
        "items": [
            {
                "type": "input",
                "messageKey": "TFL_API_KEY",
                "defaultValue": "",
                "label": "TfL API Key",
                "attributes": {
                    "placeholder": "1234abcd567890"
                }
            }
        ]
    },
    {
        "type": "submit",
        "defaultValue": "Save Settings"
    }
];
module.exports = clayConfig;
