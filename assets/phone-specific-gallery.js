const imageCredits = {
  'assets/phone-specific-gallery/e6/e6-gpu_keyboard-boot.jpg': {
    device: 'rm-609',
    description: 'Nokia E6 running Linux shell'
  },
  'assets/phone-specific-gallery/e6/e6-gpu_keyboard_modem-ping.jpg': {
    device: 'rm-609',
    description: 'First UMTS data test on E6, pinging ya.ru'
  },
  'assets/phone-specific-gallery/e6/e6-yamashell_settings.jpg': {
    device: 'rm-609',
    description: 'Nokia E6 running Yamashell, showing settings'
  },
  'https://raw.githubusercontent.com/fluffleswaffles/rapuyama-linux/e5-port/docs/devices/e5-00/photos/e5-dillo.jpg': {
    device: 'rm-632',
    description: 'Nokia E5 running Dillo web browser'
  },
  'https://raw.githubusercontent.com/fluffleswaffles/rapuyama-linux/e5-port/docs/devices/e5-00/photos/e5-fastfetch.jpg': {
    device: 'rm-632',
    description: 'Nokia E5 running fastfetch, showing WLAN'
  },
  'https://raw.githubusercontent.com/fluffleswaffles/rapuyama-linux/e5-port/docs/devices/e5-00/photos/e5-htop-fn.jpg': {
    device: 'rm-632',
    description: 'Nokia E5 running htop with fn layer on top'
  },
  'assets/phone-specific-gallery/e7/e7-test_boot.png': {
    device: 'rm-626',
    description: 'Nokia E7 running stripped down version of Yamashell'
  }
};

const gallery = document.getElementById('gallery-cards');
const galleryRoot = new URL('../', document.currentScript.src);

function createGalleryCard(path, credit) {
  const column = document.createElement('div');
  column.className = 'col-sm-6 col-xl-4';

  const card = document.createElement('figure');
  card.className = 'card h-100 mb-0';

  const link = document.createElement('a');
  link.className = 'bg-light d-flex align-items-center justify-content-center p-3';
  link.style.height = '240px';
  link.href = new URL(path, galleryRoot).href;
  link.target = '_blank';
  link.rel = 'noopener noreferrer';

  const image = document.createElement('img');
  image.className = 'img-fluid';
  image.loading = 'lazy';
  image.decoding = 'async';
  image.src = link.href;
  image.alt = credit.description;
  image.style.maxHeight = '100%';
  image.style.objectFit = 'contain';
  link.append(image);

  const caption = document.createElement('figcaption');
  caption.className = 'card-body';
  caption.textContent = credit.description;

  card.append(link, caption);
  column.append(card);
  return column;
}

if (gallery) {
  for (const [path, credit] of Object.entries(imageCredits)) {
    if (credit.device === gallery.dataset.device) {
      gallery.append(createGalleryCard(path, credit));
    }
  }
}
