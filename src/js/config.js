module.exports = [
    {
        "type": "heading",
        "defaultValue": "Pcreeps configuration"
    },
    {
        "type": "text",
        "defaultValue": "This app requires your screeps username/password. It is only saved locally to your phone."
    },
    {
        "type": "section",
        "items": [
            {
                "type": "heading",
                "defaultValue": "Authentication"
            },
            {
                "type": "input",
                "messageKey": "email",
                "defaultValue": "",
                "label": "E-Mail",
                "attributes": {
                    "type": "email",
                    "limit": "1000"
                }
            },
            {
                "type": "input",
                "messageKey": "password",
                "defaultValue": "",
                "label": "Password",
                "attributes": {
                    "type": "password",
                    "limit": "1000"
                }
            }
        ]
    },
    {
        "type": "submit",
        "defaultValue": "Save settings"
    },
];
