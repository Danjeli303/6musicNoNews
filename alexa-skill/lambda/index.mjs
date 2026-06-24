const STREAM_URL = process.env.STREAM_URL || 'https://REPLACE_WITH_PUBLIC_HOST/hls/radio6music_noNews_fip_plex.m3u8';
const STREAM_TOKEN = 'the-radio-live-stream';

function audioPlayerResponse() {
  return {
    version: '1.0',
    response: {
      shouldEndSession: true,
      directives: [
        {
          type: 'AudioPlayer.Play',
          playBehavior: 'REPLACE_ALL',
          audioItem: {
            stream: {
              token: STREAM_TOKEN,
              url: STREAM_URL,
              offsetInMilliseconds: 0
            }
          }
        }
      ]
    }
  };
}

function emptyResponse() {
  return {
    version: '1.0',
    response: {
      shouldEndSession: true
    }
  };
}

export async function handler(event) {
  const request = event && event.request ? event.request : {};

  if (request.type === 'LaunchRequest') {
    return audioPlayerResponse();
  }

  if (request.type === 'IntentRequest') {
    const intentName = request.intent && request.intent.name;

    if (intentName === 'PlayRadioIntent' || intentName === 'AMAZON.ResumeIntent') {
      return audioPlayerResponse();
    }

    if (
      intentName === 'AMAZON.CancelIntent' ||
      intentName === 'AMAZON.StopIntent' ||
      intentName === 'AMAZON.PauseIntent'
    ) {
      return emptyResponse();
    }
  }

  if (request.type && request.type.indexOf('AudioPlayer.') === 0) {
    return emptyResponse();
  }

  return audioPlayerResponse();
}
