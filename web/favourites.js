(function () {
  "use strict";

  const STORAGE_KEY = "skipper.favouriteTracks.v1";

  function clean(value) {
    return typeof value === "string" ? value.trim() : "";
  }

  function trackKey(track) {
    const artist = clean(track?.artist?.name || track?.artist).toLocaleLowerCase();
    const name = clean(track?.name || track?.title).toLocaleLowerCase();
    return `${artist}\u0000${name}`;
  }

  function lastFmSearchUrl(track) {
    const artist = clean(track?.artist?.name || track?.artist);
    const name = clean(track?.name || track?.title);
    return `https://www.last.fm/search?q=${encodeURIComponent(`${artist} ${name}`.trim())}`;
  }

  function normalize(track) {
    const artist = clean(track?.artist?.name || track?.artist);
    const name = clean(track?.name || track?.title);
    if (!artist && !name) return null;
    const imageUrl = clean(
      track?.image_url ||
        (Array.isArray(track?.image)
          ? track.image.find((image) => clean(image?.["#text"]))?.["#text"]
          : ""),
    );
    const existingSavedAt = Number(track?.date?.uts);
    const savedAt = Number.isFinite(existingSavedAt) && existingSavedAt > 0
      ? Math.floor(existingSavedAt)
      : Math.floor(Date.now() / 1000);
    return {
      name,
      mbid: clean(track?.mbid),
      url: lastFmSearchUrl({ artist, name }),
      artist: {
        name: artist,
        mbid: clean(track?.artist?.mbid),
        url: "",
      },
      album: {
        title: clean(track?.album?.title || track?.album),
        mbid: clean(track?.album?.mbid),
      },
      image: imageUrl
        ? [
            { size: "small", "#text": imageUrl },
            { size: "large", "#text": imageUrl },
          ]
        : [],
      date: {
        uts: String(savedAt),
        "#text": new Date(savedAt * 1000).toISOString(),
      },
      source: clean(track?.source || "BBC Radio 6 Music"),
      programme_pid: clean(track?.programme_pid),
      programme: clean(track?.programme),
      presenter: clean(track?.presenter),
    };
  }

  function read() {
    try {
      const value = JSON.parse(localStorage.getItem(STORAGE_KEY) || "[]");
      return Array.isArray(value) ? value.map(normalize).filter(Boolean) : [];
    } catch (_error) {
      return [];
    }
  }

  function write(tracks) {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(tracks));
    window.dispatchEvent(new CustomEvent("skipper:favourites-changed"));
  }

  function has(track) {
    const key = trackKey(track);
    return Boolean(key && read().some((item) => trackKey(item) === key));
  }

  function toggle(track) {
    const normalized = normalize(track);
    if (!normalized) return false;
    const key = trackKey(normalized);
    const tracks = read();
    const index = tracks.findIndex((item) => trackKey(item) === key);
    if (index >= 0) {
      tracks.splice(index, 1);
      write(tracks);
      return false;
    }
    tracks.unshift(normalized);
    write(tracks);
    return true;
  }

  window.SkipperFavourites = {
    STORAGE_KEY,
    has,
    lastFmSearchUrl,
    normalize,
    read,
    toggle,
  };
})();
