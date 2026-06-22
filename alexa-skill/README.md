# Alexa Skill Scaffold

This is a minimal custom Alexa skill for the deployed stream.

The Lambda handler has no third-party dependencies. It returns an
`AudioPlayer.Play` directive for the URL in the `STREAM_URL` environment
variable.

## Stream URL

After the AWS Docker stack is running, set:

```
STREAM_URL=https://PUBLIC_HOST/the-radio.mp3
```

For the temporary DNS setup, that usually looks like:

```
STREAM_URL=https://203.0.113.10.sslip.io/the-radio.mp3
```

Replace `203.0.113.10` with the EC2 public IPv4 address.

## Alexa Developer Console Setup

1. Create a new custom skill named `Six Music Skipper`.
2. Use the invocation name `six music skipper`.
3. Enable the Audio Player interface.
4. Create a Node.js Lambda function.
5. If the Lambda file is `index.mjs`, paste `lambda/index.mjs`. If the Lambda
   file is `index.js`, paste `lambda/index.js`.
6. Set the Lambda environment variable `STREAM_URL`.
7. Replace the endpoint ARN in `skill-package/skill.json` if using ASK CLI.
8. Use the interaction model in `skill-package/interactionModels/custom/en-GB.json`.
9. Build the model and test with:

```
Alexa, open Six Music Skipper
```

You can also test:

```
Alexa, ask Six Music Skipper to play
```

## Routine Phrase

If Alexa treats `Alexa, play The Radio` as a music-service request, create an
Alexa Routine:

1. When this happens: voice phrase `play The Radio`.
2. Alexa will: custom action `open Six Music Skipper`.

That makes the routine phrase simple while keeping the skill invocation reliable.
