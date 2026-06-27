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
                    "placeholder": "655c2b6bf1a8497ea9e72ec6e80420fa"
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
