const favouritesList = document.querySelector("#favourites-list");
const favouritesEmpty = document.querySelector("#favourites-empty");

function favouriteArtwork(track) {
  const images = Array.isArray(track.image) ? track.image : [];
  return images.find((image) => image?.["#text"])?.["#text"] || "";
}

function renderFavourites() {
  const tracks = window.SkipperFavourites.read();
  favouritesList.replaceChildren(
    ...tracks.map((track) => {
      const item = document.createElement("article");
      item.className = "favourite-item";
      const artwork = document.createElement("div");
      artwork.className = "favourite-artwork";
      const imageUrl = favouriteArtwork(track);
      if (imageUrl) {
        const image = document.createElement("img");
        image.src = imageUrl;
        image.alt = "";
        image.loading = "lazy";
        artwork.append(image);
      } else {
        artwork.textContent = "6";
      }
      const copy = document.createElement("div");
      copy.className = "favourite-copy";
      const title = document.createElement("a");
      title.href = track.url || window.SkipperFavourites.lastFmSearchUrl(track);
      title.target = "_blank";
      title.rel = "noopener";
      title.textContent = track.name || "Unknown track";
      const artist = document.createElement("p");
      artist.textContent = track.artist?.name || "Unknown artist";
      const saved = document.createElement("small");
      const date = new Date(Number(track.date?.uts) * 1000);
      saved.textContent = Number.isNaN(date.getTime())
        ? track.source
        : `Saved ${date.toLocaleString()} · ${track.source}`;
      copy.append(title, artist, saved);
      const remove = document.createElement("button");
      remove.className = "favourite-button is-favourite";
      remove.type = "button";
      remove.title = "Remove from favourites";
      remove.setAttribute("aria-label", `Remove ${track.name} by ${track.artist?.name} from favourites`);
      remove.innerHTML = '<span aria-hidden="true">♥</span>';
      remove.addEventListener("click", () => {
        window.SkipperFavourites.toggle(track);
        renderFavourites();
      });
      item.append(artwork, copy, remove);
      return item;
    }),
  );
  favouritesEmpty.hidden = tracks.length > 0;
}

window.addEventListener("storage", renderFavourites);
window.addEventListener("skipper:favourites-changed", renderFavourites);
renderFavourites();
