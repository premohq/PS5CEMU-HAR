// PS5CEMU-HAR UI redesign mockups: the controller glyphs and the stand-in covers. The covers are
// drawn here (a gradient, a motif and the name) because the mockups ship no real box art: in the app
// they are GameTDB's covers.

const GLYPHS = {
	cross: '<path d="M8 8 L24 24 M24 8 L8 24" stroke="currentColor" stroke-width="3.2" stroke-linecap="round"/>',
	circle: '<circle cx="16" cy="16" r="9.5" fill="none" stroke="currentColor" stroke-width="3.2"/>',
	square: '<rect x="7.5" y="7.5" width="17" height="17" rx="2" fill="none" stroke="currentColor" stroke-width="3.2"/>',
	triangle: '<path d="M16 6.5 L26 24.5 L6 24.5 Z" fill="none" stroke="currentColor" stroke-width="3.2" stroke-linejoin="round"/>',
	options: '<path d="M8 10 H24 M8 16 H24 M8 22 H24" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>',
	touchpad: '<rect x="4" y="9" width="24" height="14" rx="4" fill="none" stroke="currentColor" stroke-width="3"/>',
	updown: '<path d="M16 5 V27 M10 11 L16 5 L22 11 M10 21 L16 27 L22 21" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>',
	leftright: '<path d="M5 16 H27 M11 10 L5 16 L11 22 M21 10 L27 16 L21 22" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>',
	l1: '<rect x="2" y="7" width="28" height="18" rx="6" fill="none" stroke="currentColor" stroke-width="2.4"/><text x="16" y="20.5" font-family="Lexend" font-weight="700" font-size="11" text-anchor="middle" fill="currentColor">L1</text>',
	r1: '<rect x="2" y="7" width="28" height="18" rx="6" fill="none" stroke="currentColor" stroke-width="2.4"/><text x="16" y="20.5" font-family="Lexend" font-weight="700" font-size="11" text-anchor="middle" fill="currentColor">R1</text>',
	l2: '<path d="M4 25 V13 a7 7 0 0 1 7 -7 H25 a3 3 0 0 1 3 3 V25 Z" fill="none" stroke="currentColor" stroke-width="2.4"/><text x="16" y="21" font-family="Lexend" font-weight="700" font-size="11" text-anchor="middle" fill="currentColor">L2</text>',
	r2: '<path d="M28 25 V13 a7 7 0 0 0 -7 -7 H7 a3 3 0 0 0 -3 3 V25 Z" fill="none" stroke="currentColor" stroke-width="2.4"/><text x="16" y="21" font-family="Lexend" font-weight="700" font-size="11" text-anchor="middle" fill="currentColor">R2</text>',
	play: '<path d="M11 7 L25 16 L11 25 Z" fill="currentColor"/>',
	gear: '<circle cx="16" cy="16" r="4.5" fill="none" stroke="currentColor" stroke-width="2.8"/><path d="M16 4 V8 M16 24 V28 M4 16 H8 M24 16 H28 M7.5 7.5 L10.3 10.3 M21.7 21.7 L24.5 24.5 M7.5 24.5 L10.3 21.7 M21.7 10.3 L24.5 7.5" stroke="currentColor" stroke-width="2.8" stroke-linecap="round"/>',
	more: '<circle cx="8" cy="16" r="2.6" fill="currentColor"/><circle cx="16" cy="16" r="2.6" fill="currentColor"/><circle cx="24" cy="16" r="2.6" fill="currentColor"/>',
	check: '<path d="M7 16.5 L13.5 23 L25 10" fill="none" stroke="currentColor" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"/>',
	warn: '<path d="M16 5 L28 26 H4 Z" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linejoin="round"/><path d="M16 13 V18.5" stroke="currentColor" stroke-width="3" stroke-linecap="round"/><circle cx="16" cy="22.4" r="1.7" fill="currentColor"/>',
	dash: '<path d="M9 16 H23" stroke="currentColor" stroke-width="3.2" stroke-linecap="round"/>',
	resume: '<path d="M11 7 L25 16 L11 25 Z" fill="currentColor"/>',
	save: '<path d="M7 6 H21 L26 11 V26 H7 Z" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linejoin="round"/><path d="M11 6 V12 H20 V6 M11 26 V18 H22 V26" fill="none" stroke="currentColor" stroke-width="2.4"/>',
	load: '<path d="M16 5 V19 M10 13 L16 19 L22 13" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M6 22 V26 H26 V22" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/>',
	screens: '<rect x="6" y="4" width="20" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="2.6"/><rect x="9" y="19" width="14" height="9" rx="2" fill="none" stroke="currentColor" stroke-width="2.6"/>',
	quit: '<path d="M13 6 H7 V26 H13" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M14 16 H27 M22 11 L27 16 L22 21" fill="none" stroke="currentColor" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/>',
	chevron: '<path d="M12 7 L21 16 L12 25" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>',
	search: '<circle cx="14" cy="14" r="8" fill="none" stroke="currentColor" stroke-width="2.8"/><path d="M20 20 L27 27" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>',
	wifi: '<path d="M4 13 a17 17 0 0 1 24 0 M8.5 17.5 a11 11 0 0 1 15 0 M13 22 a4.5 4.5 0 0 1 6 0" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round"/><circle cx="16" cy="25.5" r="1.8" fill="currentColor"/>',
	globe: '<circle cx="16" cy="16" r="11" fill="none" stroke="currentColor" stroke-width="2.4"/><ellipse cx="16" cy="16" rx="4.8" ry="11" fill="none" stroke="currentColor" stroke-width="2.2"/><path d="M5 16 H27 M7.5 10.5 H24.5 M7.5 21.5 H24.5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
	sort: '<path d="M6 9 H26 M9 16 H23 M13 23 H19" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>',
};

function glyph(name, size) {
	const s = size || 28;
	return `<svg viewBox="0 0 32 32" width="${s}" height="${s}" aria-hidden="true">${GLYPHS[name] || ''}</svg>`;
}

// The stand-in covers: two colours, a motif, the name.
const GAMES = {
	botw: { name: 'The Legend of Zelda: Breath of the Wild', a: '#0f5f74', b: '#9ad3a8', motif: 'hills', sys: 'wiiu' },
	mk8: { name: 'Mario Kart 8', a: '#b3122b', b: '#ffb21e', motif: 'rays', sys: 'wiiu' },
	splatoon: { name: 'Splatoon', a: '#31107a', b: '#c7ff2a', motif: 'blobs', sys: 'wiiu' },
	xcx: { name: 'Xenoblade Chronicles X', a: '#04172e', b: '#3fc7ff', motif: 'planet', sys: 'wiiu' },
	sm3dw: { name: 'Super Mario 3D World', a: '#1b62d6', b: '#ffd23a', motif: 'stars', sys: 'wiiu' },
	bayo2: { name: 'Bayonetta 2', a: '#1d0b2b', b: '#d14cff', motif: 'rays', sys: 'wiiu' },
	pikmin3: { name: 'Pikmin 3', a: '#16461d', b: '#ffcf4a', motif: 'hills', sys: 'wiiu' },
	tww: { name: 'The Legend of Zelda: The Wind Waker HD', a: '#0b4ea0', b: '#ffe9a8', motif: 'waves', sys: 'wiiu' },
	dkc: { name: 'Donkey Kong Country: Tropical Freeze', a: '#0c6c86', b: '#ff8a2a', motif: 'hills', sys: 'wiiu' },
	toad: { name: 'Captain Toad: Treasure Tracker', a: '#5a2ea6', b: '#ffcb47', motif: 'stars', sys: 'wiiu' },
	pkx: { name: 'Pokémon X', a: '#0a1840', b: '#e8323a', motif: 'planet', sys: 'n3ds' },
	albw: { name: 'The Legend of Zelda: A Link Between Worlds', a: '#3a1460', b: '#ffcd52', motif: 'rays', sys: 'n3ds' },
	fea: { name: 'Fire Emblem Awakening', a: '#0d1d4a', b: '#e9b44c', motif: 'rays', sys: 'n3ds' },
	acnl: { name: 'Animal Crossing: New Leaf', a: '#2f8f3a', b: '#ffe1a8', motif: 'blobs', sys: 'n3ds' },
	mh4u: { name: 'Monster Hunter 4 Ultimate', a: '#2a1406', b: '#ff7a1a', motif: 'planet', sys: 'n3ds' },
	luigi: { name: "Luigi's Mansion: Dark Moon", a: '#0b2a2a', b: '#62ffb0', motif: 'stars', sys: 'n3ds' },
	icarus: { name: 'Kid Icarus: Uprising', a: '#1c63c9', b: '#ffffff', motif: 'rays', sys: 'n3ds' },
	sm3dl: { name: 'Super Mario 3D Land', a: '#c8102e', b: '#ffd700', motif: 'stars', sys: 'n3ds' },
	yokai: { name: '妖怪ウォッチ2 真打', a: '#3b1a6e', b: '#ff5fa2', motif: 'blobs', sys: 'n3ds' },
	bravely: { name: 'Bravely Default', a: '#1b2a4a', b: '#f2d38a', motif: 'rays', sys: 'n3ds' },
	kirby: { name: 'Kirby: Planet Robobot', a: '#e85a9a', b: '#ffe1f0', motif: 'stars', sys: 'n3ds' },
	mk7: { name: 'Mario Kart 7', a: '#0c3e8c', b: '#ff3b30', motif: 'rays', sys: 'n3ds' },
	oras: { name: 'Pokémon Omega Ruby', a: '#5a0b0b', b: '#ff5a3c', motif: 'planet', sys: 'n3ds' },
	smash: { name: 'Super Smash Bros. for Nintendo 3DS', a: '#101820', b: '#ffcc33', motif: 'rays', sys: 'n3ds' },
	oot3d: { name: 'The Legend of Zelda: Ocarina of Time 3D', a: '#0e3b2e', b: '#ffd36b', motif: 'hills', sys: 'n3ds' },
	xc3d: { name: 'Xenoblade Chronicles 3D', a: '#0a2140', b: '#7fd0ff', motif: 'planet', sys: 'n3ds' },
};

function motif(kind, a, b) {
	switch (kind) {
	case 'hills': return `radial-gradient(120% 60% at 30% 100%, ${b} 0 38%, transparent 39%), radial-gradient(110% 55% at 90% 100%, ${b}cc 0 42%, transparent 43%), linear-gradient(180deg, ${a}, ${b}66)`;
	case 'rays': return `repeating-conic-gradient(from 0deg at 50% 70%, ${b}33 0 8deg, transparent 8deg 22deg), radial-gradient(70% 55% at 50% 70%, ${b}cc, transparent 70%), linear-gradient(180deg, ${a}, ${a})`;
	case 'blobs': return `radial-gradient(30% 22% at 28% 30%, ${b} 0 98%, transparent), radial-gradient(26% 20% at 72% 62%, ${b}dd 0 98%, transparent), radial-gradient(18% 14% at 40% 78%, ${b}bb 0 98%, transparent), linear-gradient(160deg, ${a}, ${a})`;
	case 'planet': return `radial-gradient(50% 36% at 62% 40%, ${b} 0 40%, ${b}44 41% 47%, transparent 48%), radial-gradient(1px 1px at 20% 20%, #fff, transparent), linear-gradient(180deg, ${a}, #000)`;
	case 'stars': return `radial-gradient(9% 7% at 25% 30%, ${b} 0 60%, transparent 62%), radial-gradient(7% 5% at 70% 22%, ${b} 0 60%, transparent 62%), radial-gradient(12% 9% at 60% 60%, ${b} 0 60%, transparent 62%), linear-gradient(180deg, ${a}, ${a}cc)`;
	case 'waves': return `repeating-radial-gradient(120% 60% at 50% 120%, ${b}55 0 6%, transparent 6% 12%), linear-gradient(180deg, ${a}, ${b}88)`;
	}
	return `linear-gradient(${a}, ${b})`;
}

function paintCovers() {
	document.querySelectorAll('[data-game]').forEach((el) => {
		const g = GAMES[el.dataset.game];
		if (!g) return;
		el.classList.add('cover', g.sys);
		const h = el.offsetHeight;
		const size = Math.max(14, Math.round(h * (g.sys === 'wiiu' ? 0.072 : 0.085)));
		el.innerHTML = `<div class="band">${g.sys === 'wiiu' ? 'Wii U' : 'NINTENDO 3DS'}</div><div class="art" style="background:${motif(g.motif, g.a, g.b)}"></div><div class="name" style="font-size:${size}px">${g.name}</div>`;
		const band = el.querySelector('.band');
		band.style.fontSize = Math.max(8, Math.round(h * 0.04)) + 'px';
		el.style.setProperty('--glow', g.b + '77');
	});
	document.querySelectorAll('[data-glyph]').forEach((el) => {
		el.innerHTML = glyph(el.dataset.glyph, el.dataset.size ? +el.dataset.size : undefined);
	});
}

document.addEventListener('DOMContentLoaded', paintCovers);

// Stand-ins for the backdrops: a Wii U game's boot screen (meta/bootTvTex.tga, 1280 x 720) and a 3DS
// game's top-screen screenshot (400 x 240 native). The app shows the real ones; these are drawn.
function hills(ctx, w, h, y, amp, colour, seed) {
	ctx.fillStyle = colour;
	ctx.beginPath();
	ctx.moveTo(0, h);
	for (let x = 0; x <= w; x += w / 48) {
		const t = x / w;
		ctx.lineTo(x, y - amp * (0.55 * Math.sin(t * 7.1 + seed) + 0.3 * Math.sin(t * 17.3 + seed * 2) + 0.15 * Math.sin(t * 41 + seed * 3)));
	}
	ctx.lineTo(w, h);
	ctx.closePath();
	ctx.fill();
}

const BACKDROPS = {
	'boot:botw': (ctx, w, h) => {
		const sky = ctx.createLinearGradient(0, 0, 0, h);
		sky.addColorStop(0, '#7fb9d6'); sky.addColorStop(0.55, '#e9e3c8'); sky.addColorStop(1, '#f2d9a6');
		ctx.fillStyle = sky; ctx.fillRect(0, 0, w, h);
		const sun = ctx.createRadialGradient(w * 0.72, h * 0.42, 10, w * 0.72, h * 0.42, w * 0.35);
		sun.addColorStop(0, 'rgba(255,248,220,.95)'); sun.addColorStop(1, 'rgba(255,248,220,0)');
		ctx.fillStyle = sun; ctx.fillRect(0, 0, w, h);
		hills(ctx, w, h, h * 0.56, 70, '#9fb7c4', 1.3);
		hills(ctx, w, h, h * 0.64, 46, '#7d9fa0', 2.1);
		hills(ctx, w, h, h * 0.74, 38, '#4f7d5f', 0.4);
		hills(ctx, w, h, h * 0.86, 30, '#2f5a3c', 3.3);
		ctx.fillStyle = '#1d3a28'; // a figure on the ridge
		ctx.fillRect(w * 0.3, h * 0.76, 6, 18); ctx.beginPath(); ctx.arc(w * 0.3 + 3, h * 0.76 - 4, 5, 0, 7); ctx.fill();
		ctx.textAlign = 'center'; ctx.fillStyle = 'rgba(255,255,255,.96)'; ctx.shadowColor = 'rgba(0,0,0,.35)'; ctx.shadowBlur = 18;
		ctx.font = '600 30px Lexend'; ctx.fillText('THE LEGEND OF ZELDA', w * 0.5, h * 0.3);
		ctx.font = '700 64px Lexend'; ctx.fillText('BREATH OF THE WILD', w * 0.5, h * 0.3 + 70);
		ctx.shadowBlur = 0;
	},
	'shot:pkx': (ctx, w, h) => {
		const sky = ctx.createLinearGradient(0, 0, 0, h * 0.6);
		sky.addColorStop(0, '#5fa8f0'); sky.addColorStop(1, '#bfe3ff');
		ctx.fillStyle = sky; ctx.fillRect(0, 0, w, h);
		ctx.fillStyle = '#8fd07a'; ctx.fillRect(0, h * 0.55, w, h * 0.45);
		ctx.fillStyle = '#d9c9a3'; ctx.beginPath(); ctx.moveTo(w * 0.42, h); ctx.lineTo(w * 0.5, h * 0.55); ctx.lineTo(w * 0.56, h * 0.55); ctx.lineTo(w * 0.7, h); ctx.fill();
		const houses = [[30, 95, 70, 50, '#e8e2d6', '#c0392b'], [120, 105, 60, 42, '#f1ead8', '#2e6fb5'], [270, 92, 80, 56, '#efe6d2', '#8e44ad'], [340, 110, 50, 36, '#f4efe3', '#d35400']];
		for (const [x, y, hw, hh, wall, roof] of houses) {
			ctx.fillStyle = wall; ctx.fillRect(x, y, hw, hh);
			ctx.fillStyle = roof; ctx.beginPath(); ctx.moveTo(x - 6, y); ctx.lineTo(x + hw / 2, y - 22); ctx.lineTo(x + hw + 6, y); ctx.fill();
			ctx.fillStyle = '#5a4a3a'; ctx.fillRect(x + hw / 2 - 6, y + hh - 16, 12, 16);
		}
		ctx.fillStyle = '#2d6a3e'; for (const x of [10, 105, 190, 250, 385]) { ctx.beginPath(); ctx.arc(x, h * 0.55, 14, 0, 7); ctx.fill(); }
		ctx.fillStyle = '#e8323a'; ctx.fillRect(w * 0.52, h * 0.7, 10, 8); ctx.fillStyle = '#1b2a6b'; ctx.fillRect(w * 0.52, h * 0.7 + 8, 10, 12);
		ctx.fillStyle = '#ffd36b'; ctx.fillRect(w * 0.52 + 2, h * 0.7 - 6, 6, 6);
	},
};

function paintBackdrops() {
	document.querySelectorAll('canvas[data-backdrop]').forEach((canvas) => {
		const draw = BACKDROPS[canvas.dataset.backdrop];
		if (!draw) return;
		const shot = canvas.dataset.backdrop.startsWith('shot:');
		canvas.width = shot ? 400 : 1280;
		canvas.height = shot ? 240 : 720;
		draw(canvas.getContext('2d'), canvas.width, canvas.height);
	});
}

document.addEventListener('DOMContentLoaded', () => document.fonts.ready.then(paintBackdrops));
