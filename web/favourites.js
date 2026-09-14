(function () {
  "use strict";

  const STORAGE_KEY = "skipper.favouriteTracks.v1";
  const API_URL = "/api/favourites";
  let tracks = [];
  let loadRequest;

  function clean(value) {
    return typeof value === "string" ? value.trim() : "";
  }

  function trackKey(track) {
    const artist = clean(track?.artist?.name || track?.artist).toLocaleLowerCase();
    const name = clean(track?.name || track?.title).toLocaleLowerCase();
    return `${artist}\u0000${name}`;
  }

  function webSearchUrl(track) {
    const artist = clean(track?.artist?.name || track?.artist);
    const name = clean(track?.name || track?.title);
    return `https://www.google.com/search?q=${encodeURIComponent(`${artist} ${name}`.trim())}`;
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
      url: webSearchUrl({ artist, name }),
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

  function readLegacy() {
    try {
      const value = JSON.parse(localStorage.getItem(STORAGE_KEY) || "[]");
      return Array.isArray(value) ? value.map(normalize).filter(Boolean) : [];
    } catch (_error) {
      return [];
    }
  }

  tracks = readLegacy();

  function changed(error = "") {
    window.dispatchEvent(
      new CustomEvent("skipper:favourites-changed", { detail: { error } }),
    );
  }

  function read() {
    return tracks.slice();
  }

  async function request(method, track) {
    const response = await fetch(API_URL, {
      method,
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(track),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(data.error || "Favourites could not be updated.");
    }
    return data;
  }

  function load(force = false) {
    if (loadRequest && !force) return loadRequest;
    loadRequest = (async () => {
      const response = await fetch(API_URL, { cache: "no-store" });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || !Array.isArray(data.tracks)) {
        throw new Error(data.error || "Favourites could not be loaded.");
      }
      const serverTracks = data.tracks.map(normalize).filter(Boolean);
      const legacyTracks = readLegacy().filter(
        (track) => !serverTracks.some((item) => trackKey(item) === trackKey(track)),
      );
      if (legacyTracks.length) {
        await Promise.all(legacyTracks.map((track) => request("POST", track)));
      }
      tracks = [...legacyTracks, ...serverTracks];
      try {
        localStorage.removeItem(STORAGE_KEY);
      } catch (_error) {
        // Server favourites still work when Safari blocks local storage access.
      }
      changed();
      return read();
    })().catch((error) => {
      changed(error.message);
      return read();
    });
    return loadRequest;
  }

  function write(nextTracks) {
    tracks = nextTracks;
    window.dispatchEvent(new CustomEvent("skipper:favourites-changed"));
  }

  function has(track) {
    const key = trackKey(track);
    return Boolean(key && tracks.some((item) => trackKey(item) === key));
  }

  function toggle(track) {
    const normalized = normalize(track);
    if (!normalized) return false;
    const key = trackKey(normalized);
    const previous = tracks.slice();
    const nextTracks = tracks.slice();
    const index = nextTracks.findIndex((item) => trackKey(item) === key);
    if (index >= 0) {
      nextTracks.splice(index, 1);
      write(nextTracks);
      request("DELETE", normalized).catch((error) => {
        write(previous);
        changed(error.message);
      });
      return false;
    }
    nextTracks.unshift(normalized);
    write(nextTracks);
    request("POST", normalized).catch((error) => {
      write(previous);
      changed(error.message);
    });
    return true;
  }

  window.SkipperFavourites = {
    STORAGE_KEY,
    has,
    load,
    webSearchUrl,
    normalize,
    read,
    toggle,
  };
})();
