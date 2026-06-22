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

1. Create a new custom skill named `The Radio`.
2. Use the invocation name `the radio`.
3. Enable the Audio Player interface.
4. Create a Node.js Lambda function and paste `lambda/index.js`.
5. Set the Lambda environment variable `STREAM_URL`.
6. Replace the endpoint ARN in `skill-package/skill.json` if using ASK CLI.
7. Use the interaction model in `skill-package/interactionModels/custom/en-GB.json`.
8. Build the model and test with:

```
Alexa, open The Radio
```

You can also test:

```
Alexa, ask The Radio to play
```

## Routine Phrase

If Alexa treats `Alexa, play The Radio` as a music-service request, create an
Alexa Routine:

1. When this happens: voice phrase `play The Radio`.
2. Alexa will: custom action `open The Radio`.

That makes the routine phrase simple while keeping the skill invocation reliable.
