'use strict';

/* ================= 基础 ================= */
const $ = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));

/* ================= 极简线条图标（1.6px 描边） ================= */
const ICONS = {
	box: '<path d="M21 8a2 2 0 0 0-1.06-1.77l-7-3.93a2 2 0 0 0-1.88 0l-7 3.93A2 2 0 0 0 3 8v8a2 2 0 0 0 1.06 1.77l7 3.93a2 2 0 0 0 1.88 0l7-3.93A2 2 0 0 0 21 16Z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/>',
	folder: '<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"/>',
	file: '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/>',
	trash: '<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>',
	wifi: '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
	message: '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
	clock: '<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>',
	upload: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="m7 8 5-5 5 5"/><path d="M12 3v12"/>',
	scan: '<path d="M3 7V5a2 2 0 0 1 2-2h2"/><path d="M17 3h2a2 2 0 0 1 2 2v2"/><path d="M21 17v2a2 2 0 0 1-2 2h-2"/><path d="M7 21H5a2 2 0 0 1-2-2v-2"/><path d="M7 12h10"/>',
	clipboard: '<path d="M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2"/><rect x="8" y="2" width="8" height="4" rx="1"/>',
	plus: '<path d="M12 5v14"/><path d="M5 12h14"/>',
	refresh: '<path d="M3 12a9 9 0 0 1 15-6.7L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-15 6.7L3 16"/><path d="M3 21v-5h5"/>',
	monitor: '<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8"/><path d="M12 17v4"/>',
};

function icon(name, size = 16) {
	return `<svg class="ic" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name]}</svg>`;
}

/* 静态 HTML 里的 <i data-icon="folder"> 占位符替换 */
$$('i[data-icon]').forEach((el) => {
	el.outerHTML = icon(el.dataset.icon, el.dataset.size || 15);
});

const state = {
	info: null,
	cwd: '',
	entries: [],
	sort: 'name',
	desc: false,
	filter: '',
	tab: 'files',
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* complete 响应丢失时重试，避免"已传完却报失败" */
async function retryJSON(fn, tries = 3) {
	let err = null;
	for (let i = 0; i < tries; i++) {
		try {
			return await fn();
		} catch (e) {
			err = e;
			await sleep(800);
		}
	}
	throw err;
}

async function fetchJSON(url, opts = {}) {
	const res = await fetch(url, opts);
	let j = {};
	try { j = await res.json(); } catch (_) { /* ignore */ }
	if (!res.ok) {
		throw new Error(j.error || `请求失败 (${res.status})`);
	}
	return j;
}
const jget = (u) => fetchJSON(u);
const jpost = (u, body) => fetchJSON(u, {
	method: 'POST',
	headers: { 'Content-Type': 'application/json' },
	body: JSON.stringify(body || {}),
});
const jdel = (u) => fetchJSON(u, { method: 'DELETE' });

function fmtBytes(n) {
	if (n === 0 || n == null) return '0 B';
	const units = ['B', 'KB', 'MB', 'GB', 'TB'];
	let i = 0;
	while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
	return (i === 0 ? n : n.toFixed(n >= 100 ? 0 : 1)) + ' ' + units[i];
}

function fmtTime(ts) {
	if (!ts) return '';
	const d = new Date(ts * 1000);
	const p = (x) => String(x).padStart(2, '0');
	return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

function escapeHTML(s) {
	return s.replace(/[&<>"']/g, (c) => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
	}[c]));
}

let toastTimer = null;
function toast(msg, type = 'ok') {
	const t = $('#toast');
	t.textContent = msg;
	t.className = 'toast ' + type;
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => t.classList.add('hidden'), 2600);
}

function confirmBox(text) {
	return new Promise((resolve) => {
		const modal = $('#modal');
		$('#modalText').textContent = text;
		modal.classList.remove('hidden');
		const done = (v) => {
			modal.classList.add('hidden');
			$('#modalOk').onclick = null;
			$('#modalCancel').onclick = null;
			resolve(v);
		};
		$('#modalOk').onclick = () => done(true);
		$('#modalCancel').onclick = () => done(false);
	});
}

/* 剪贴板：非 https 环境回退到 execCommand */
async function copyText(text) {
	if (navigator.clipboard && window.isSecureContext) {
		try { await navigator.clipboard.writeText(text); return true; } catch (_) {}
	}
	const ta = document.createElement('textarea');
	ta.value = text;
	ta.style.cssText = 'position:fixed;opacity:0;left:-9999px';
	document.body.appendChild(ta);
	ta.focus();
	ta.select();
	let ok = false;
	try { ok = document.execCommand('copy'); } catch (_) {}
	ta.remove();
	return ok;
}

async function pickClipboard() {
	if (navigator.clipboard && navigator.clipboard.readText) {
		try { return await navigator.clipboard.readText(); } catch (_) {}
	}
	return null;
}

/* ================= 标签页 ================= */
$$('#tabs button').forEach((btn) => {
	btn.addEventListener('click', () => {
		$$('#tabs button').forEach((b) => b.classList.remove('active'));
		btn.classList.add('active');
		state.tab = btn.dataset.tab;
		$$('.view').forEach((v) => v.classList.remove('active'));
		$('#view-' + state.tab).classList.add('active');
		if (state.tab === 'files') refreshListing();
		if (state.tab === 'devices') { selfDeviceQR(); loadDevices(); }
		if (state.tab === 'texts') loadTexts();
		if (state.tab === 'history') loadHistory();
	});
});

/* ================= 文件浏览 ================= */
function crumbsHTML(path) {
	const box = $('#crumbs');
	box.innerHTML = '';
	const root = document.createElement('a');
	root.textContent = '根目录';
	root.onclick = () => navigate('');
	box.appendChild(root);
	const parts = path ? path.split('/') : [];
	let acc = '';
	parts.forEach((p, i) => {
		acc = acc ? acc + '/' + p : p;
		const target = acc;
		const sep = document.createElement('span');
		sep.className = 'sep';
		sep.textContent = ' / ';
		box.appendChild(sep);
		if (i === parts.length - 1) {
			const cur = document.createElement('span');
			cur.className = 'cur';
			cur.textContent = p;
			box.appendChild(cur);
		} else {
			const a = document.createElement('a');
			a.textContent = p;
			a.onclick = () => navigate(target);
			box.appendChild(a);
		}
	});
}

/* 统一线条图标：目录 = 文件夹，文件 = 纸张 */
function fileIcon(e) {
	return icon(e.isDir ? 'folder' : 'file', 17);
}

function sortedEntries() {
	const key = state.sort;
	const desc = state.desc;
	const f = state.filter.trim().toLowerCase();
	let list = state.entries.filter((e) => !f || e.name.toLowerCase().includes(f));
	list.sort((a, b) => {
		if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
		let r = 0;
		if (key === 'name') r = a.name.localeCompare(b.name, 'zh');
		else if (key === 'size') r = a.size - b.size;
		else r = a.mtime - b.mtime;
		return desc ? -r : r;
	});
	return list;
}

async function navigate(path) {
	state.cwd = path;
	$('#dzPath').textContent = path ? path : '根目录';
	await refreshListing();
}

async function refreshListing() {
	const path = state.cwd;
	try {
		const j = await jget('/api/list?path=' + encodeURIComponent(path));
		if (j.path === state.cwd) { // 防止快速切换时错乱
			state.entries = j.entries;
			renderFiles();
		}
	} catch (e) {
		toast(e.message, 'err');
	}
}

function renderFiles() {
	crumbsHTML(state.cwd);
	const table = $('#fileTable');
	table.innerHTML = '';

	const head = document.createElement('div');
	head.className = 'file-row head';
	head.innerHTML = `
		<div>名称</div>
		<div class="h-size">大小</div>
		<div class="h-mtime">修改时间</div>
		<div style="text-align:right">操作</div>`;
	table.appendChild(head);

	const list = sortedEntries();
	const hasRows = list.length > 0;
	$('#emptyDir').classList.toggle('hidden', hasRows);

	for (const e of list) {
		const row = document.createElement('div');
		row.className = 'file-row';
		const childPath = state.cwd ? state.cwd + '/' + e.name : e.name;
		const enc = encodeURIComponent(childPath);

		const name = document.createElement('a');
		name.className = 'f-name';
		name.title = e.name;
		name.innerHTML = `<span class="icon">${fileIcon(e)}</span><span>${escapeHTML(e.name)}</span>`;
		if (e.isDir) {
			name.onclick = () => navigate(childPath);
		} else {
			name.onclick = () => confirmFileOpen(e, childPath);
		}

		const size = document.createElement('div');
		size.className = 'size';
		size.textContent = e.isDir ? '—' : fmtBytes(e.size);

		const mtime = document.createElement('div');
		mtime.className = 'mtime';
		mtime.textContent = fmtTime(e.mtime);

		const zipA = document.createElement('a');
		zipA.className = 'btn small';
		zipA.textContent = e.isDir ? '打包' : '下载';
		zipA.href = e.isDir ? `/zip/${enc}` : `/d/${enc}`;

		const delBtn = document.createElement('button');
		delBtn.className = 'btn small iconic';
		delBtn.title = '删除';
		delBtn.innerHTML = icon('trash', 13);
		delBtn.onclick = async () => {
			const ok = await confirmBox(`确定要删除「${e.name}」吗？${e.isDir ? '（目录及其内容将一并删除）' : ''}`);
			if (!ok) return;
			try {
				await jpost('/api/delete', { path: childPath });
				toast('已删除');
				refreshListing();
			} catch (err) { toast(err.message, 'err'); }
		};

		const acts = document.createElement('div');
		acts.className = 'f-actions';
		acts.appendChild(zipA);
		acts.appendChild(delBtn);
		row.appendChild(name);
		row.appendChild(size);
		row.appendChild(mtime);
		row.appendChild(acts);
		table.appendChild(row);
	}
}


function confirmFileOpen(e, childPath) {
	if (e.mtime === 0) return;
	// 在线预览几类常见格式，其余直接下载
	const n = e.name.toLowerCase();
	const enc = encodeURIComponent(childPath);
	if (/\.(jpg|jpeg|png|gif|bmp|webp|svg)$/.test(n)) {
		window.open(`/d/${enc}`, '_blank');
	} else if (/\.(txt|md|json|log|csv|yml|yaml|ini)$/.test(n) && e.size < 2 * 1024 * 1024) {
		previewText(`/d/${enc}`, e.name);
	} else if (/\.(mp4|mp3|wav|webm|ogg|m4a)$/.test(n)) {
		previewMedia(`/d/${enc}`, e.name, n);
	} else {
		location.href = `/d/${enc}`;
	}
}

let previewWin = null;
function previewText(url, title) {
	fetch(url).then((r) => r.text()).then((t) => {
		const w = window.open('', '_blank');
		if (!w) { toast('浏览器拦截了弹出窗口', 'err'); return; }
		const pre = document.createElement('pre');
		pre.style.cssText = 'white-space:pre-wrap;word-break:break-word;padding:16px;font-size:13px;';
		pre.textContent = t;
		w.document.body.style.cssText = 'margin:0;background:#111;color:#eee;font-family:monospace';
		w.document.title = title;
		w.document.body.appendChild(pre);
	}).catch(() => { /* network 中断时静默 */ });
}

function previewMedia(url, title, n) {
	previewWin = window.open('', '_blank');
	if (!previewWin) { toast('浏览器拦截了弹出窗口', 'err'); return; }
	const isVideo = /\.(mp4|webm)$/.test(n);
	previewWin.document.title = title;
	previewWin.document.body.style.cssText = 'margin:0;background:#000;display:flex;align-items:center;justify-content:center;height:100vh';
	const el = document.createElement(isVideo ? 'video' : 'audio');
	el.src = url;
	el.controls = true;
	el.style.maxWidth = '96vw';
	previewWin.document.body.appendChild(el);
}

$('#btnRefresh').onclick = refreshListing;
$('#sort').onchange = (e) => { state.sort = e.target.value; renderFiles(); };
$('#btnDesc').onclick = (e) => {
	state.desc = !state.desc;
	e.target.textContent = state.desc ? '↑' : '↓';
	renderFiles();
};
$('#filter').oninput = (e) => { state.filter = e.target.value; renderFiles(); };
$('#btnMkdir').onclick = async () => {
	const name = prompt('新建文件夹名称：');
	if (!name) return;
	try {
		await jpost('/api/mkdir', { dir: state.cwd, path: name });
		toast('已创建');
		refreshListing();
	} catch (e) { toast(e.message, 'err'); }
};

/* ================= 上传（分片 + 断点续传） ================= */
const CHUNK_SIZE = 4 * 1024 * 1024;
const MAX_PARALLEL = 3;

const uploadItems = [];
let activeUploads = 0;

/* 目标目录 = 当前浏览目录 + 拖入的相对路径（决定文件最终落在哪） */
function targetDirFor(relDir) {
	const base = state.cwd ? state.cwd + '/' : '';
	return (base + (relDir || '')).replace(/^\/+|\/+$/g, '');
}

function addToUploads(items) {
	for (const it of items) {
		it.state = 'pending'; // pending(待确认) | queued | run | done | err
		it.received = 0;
		it.speed = 0;
		it.samples = [];
		it.targetDir = targetDirFor(it.relDir); // 入队时锁定，避免中途切换页面跑偏
		uploadItems.push(it);
		renderUploadItem(it);
	}
	$('#uploadList').classList.remove('hidden');
	updateConfirmBar();
}

/* ---- 确认上传栏 ---- */
function updateConfirmBar() {
	const pend = uploadItems.filter((i) => i.state === 'pending');
	const bar = $('#confirmBar');
	if (!pend.length) {
		bar.classList.add('hidden');
		return;
	}
	bar.classList.remove('hidden');
	const total = pend.reduce((s, i) => s + i.file.size, 0);
	$('#confirmInfo').textContent = `已选择 ${pend.length} 个文件 · 共 ${fmtBytes(total)}，确认后才开始传输`;
}

$('#btnConfirmUpload').onclick = () => {
	uploadItems.forEach((i) => { if (i.state === 'pending') i.state = 'queued'; });
	updateConfirmBar();
	pump();
};

$('#btnClearPend').onclick = () => {
	for (const i of uploadItems.filter((i) => i.state === 'pending')) {
		i.cancelled = true;
		uploadItems.splice(uploadItems.indexOf(i), 1);
		i.dom.remove();
	}
	updateConfirmBar();
};

function pump() {
	while (activeUploads < MAX_PARALLEL) {
		const next = uploadItems.find((i) => i.state === 'queued');
		if (!next) break;
		activeUploads++;
		next.state = 'run';
		runUpload(next).finally(() => {
			activeUploads--;
			pump();
		});
	}
}

async function runUpload(item) {
	const ctl = item;
	const file = item.file;
	item.dom.querySelector('.up-state').textContent = '准备中…';
	item.dom.querySelector('.up-state').className = 'up-state run';
	let id = null;
	try {
		const res = await jpost('/api/upload/init', {
			name: file.name, size: file.size, dir: item.targetDir,
		});
		id = res.id;
		while (true) {
			if (ctl.cancelled) {
				await jpost('/api/upload/abort/' + id, {});
				item.state = 'err';
				updateUpState(item, '已取消', 'err');
				refreshListing();
				return;
			}
			let offset = item.received;
			if (offset >= file.size) break;
			try {
				const end = Math.min(offset + CHUNK_SIZE, file.size);
				const blob = file.slice(offset, end);
				const r = await fetch('/api/upload/chunk/' + id + '?offset=' + offset, {
					method: 'POST',
					headers: { 'Content-Type': 'application/octet-stream' },
					body: blob,
				});
				const j = await r.json();
				if (!r.ok) throw new Error(j.error || ('分片上传失败 ' + r.status));
				item.received = j.received; // 以服务端为准
			} catch (e) {
				// 网络失败 → 查询服务端进度，实现断点续传
				updateUpState(item, '网络中断，续传中…', 'err');
				await sleep(1200);
				try {
					const st = await jget('/api/upload/status/' + id);
					item.received = st.received;
				} catch (_) { /* 继续重试 */ }
			}
			markProgress(item);
		}
		const fin = await retryJSON(() => jpost('/api/upload/complete/' + id, {}));
		item.state = 'done';
		const fname = fin.name || file.name;
		updateUpState(item, `已接收（${fname}）`, 'done');
		if (!file.size) markProgress(item);
		toast(`「${fname}」上传完成`);
		item.dom.querySelector('.up-bar-fill').style.width = '100%';
		refreshListing();
	} catch (e) {
		item.state = 'err';
		updateUpState(item, '失败：' + e.message, 'err');
		if (id) { try { await jpost('/api/upload/abort/' + id, {}); } catch (_) {} }
	}
}

function markProgress(item) {
	const now = performance.now();
	item.samples.push({ t: now, b: item.received });
	while (item.samples.length > 12) item.samples.shift();
	const first = item.samples[0];
	let speed = 0;
	if (first && item.samples.length > 2) {
		const dt = (now - first.t) / 1000;
		if (dt > 0.2) speed = (item.received - first.b) / dt;
	}
	const pct = item.file.size ? Math.floor((item.received / item.file.size) * 100) : 100;
	item.dom.querySelector('.up-bar-fill').style.width = pct + '%';
	item.dom.querySelector('.up-size').textContent =
		`${fmtBytes(item.received)} / ${fmtBytes(item.file.size)} · ${pct}%${speed ? ' · ' + fmtBytes(speed) + '/s' : ''}`;
}

function updateUpState(item, text, cls) {
	const el = item.dom.querySelector('.up-state');
	el.textContent = text;
	el.className = 'up-state ' + (cls || 'run');
}

function renderUploadItem(item) {
	const div = document.createElement('div');
	div.className = 'upload-item';
	div.innerHTML = `
		<div class="up-head">
			<span class="up-name"><b>${escapeHTML(item.targetDir ? item.targetDir + '/' : '')}${escapeHTML(item.file.name)}</b></span>
			<span class="up-size">${fmtBytes(item.file.size)}</span>
			<span class="up-state">待确认</span>
			<button class="up-close" title="移除">✕</button>
		</div>
		<div class="up-bar"><div class="up-bar-fill"></div></div>`;
	div.querySelector('.up-close').onclick = () => {
		item.cancelled = true;
		// 运行中的项不能直接删 DOM，交给 runUpload 走 abort 流程
		if (item.state === 'pending' || item.state === 'queued' || item.state === 'err' || item.state === 'done') {
			uploadItems.splice(uploadItems.indexOf(item), 1);
			div.remove();
			updateConfirmBar();
		}
	};
	item.dom = div;
	$('#uploadList').appendChild(div);
}

/* ---- 选择文件 / 文件夹 ---- */
$('#fileInput').addEventListener('change', (e) => {
	const items = Array.from(e.target.files).map((f) => ({ file: f, relDir: '' }));
	addToUploads(items);
	e.target.value = '';
});
$('#dirInput').addEventListener('change', (e) => {
	const items = Array.from(e.target.files).map((f) => {
		// webkitRelativePath: "根/子/a.png" → 去掉第一级
		let rel = f.webkitRelativePath || f.name;
		const idx = rel.indexOf('/');
		rel = idx >= 0 ? rel.slice(idx + 1) : '';
		const lastSlash = rel.lastIndexOf('/');
		return { file: f, relDir: lastSlash >= 0 ? rel.slice(0, lastSlash) : '' };
	});
	addToUploads(items);
	e.target.value = '';
});

/* ---- 拖拽（含文件夹） ---- */
const dz = $('#dropzone');
['dragenter', 'dragover'].forEach((ev) =>
	dz.addEventListener(ev, (e) => { e.preventDefault(); dz.classList.add('drag'); }));
['dragleave', 'drop'].forEach((ev) =>
	dz.addEventListener(ev, (e) => { e.preventDefault(); dz.classList.remove('drag'); }));

dz.addEventListener('drop', async (e) => {
	e.preventDefault();
	const out = [];
	const entries = Array.from(e.dataTransfer.items || [])
		.map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry())
		.filter(Boolean);
	if (entries.length) {
		for (const en of entries) await walkEntry(en, '', out);
	} else {
		const files = Array.from(e.dataTransfer.files || []);
		for (const f of files) out.push({ file: f, relDir: '' });
	}
	if (out.length) addToUploads(out);
});

function walkEntry(entry, base, out) {
	return new Promise((resolve) => {
		if (entry.isFile) {
			entry.file((f) => {
				out.push({ file: f, relDir: base });
				resolve();
			}, () => resolve());
		} else if (entry.isDirectory) {
			const dirPath = base ? base + '/' + entry.name : entry.name;
			const reader = entry.createReader();
			const readBatch = () => {
				reader.readEntries(async (es) => {
					if (!es.length) { resolve(); return; }
					await Promise.all(es.map((e2) => walkEntry(e2, dirPath, out)));
					readBatch();
				}, () => resolve());
			};
			readBatch();
		} else {
			resolve();
		}
	});
}

/* ================= 设备 ================= */
let infoLoaded = false;

async function loadInfo() {
	if (infoLoaded) return;
	try {
		state.info = await jget('/api/info');
		infoLoaded = true;
		$('#devName').textContent = `当前服务器名称：${state.info.name}`;
		const sel = $('#ipSelect');
		sel.innerHTML = '';
		for (const u of state.info.urls) {
			const opt = document.createElement('option');
			opt.value = u;
			opt.textContent = u;
			sel.appendChild(opt);
		}
		selfDeviceQR();
	} catch (e) {
		toast(e.message, 'err');
	}
}

function selfDeviceQR() {
	if (!state.info) return;
	const u = $('#ipSelect').value || state.info.urls[0];
	$('#selfUrl').textContent = u;
	$('#qrImg').src = '/api/qr?u=' + encodeURIComponent(u);
	$('#btnDownloadQR').href = '/api/qr?u=' + encodeURIComponent(u);
}
$('#ipSelect').addEventListener('change', selfDeviceQR);
$('#btnCopySelfUrl').onclick = async () => {
	const ok = await copyText($('#selfUrl').textContent);
	toast(ok ? '地址已复制' : '复制失败，请手动选择复制', ok ? 'ok' : 'err');
};

async function loadDevices() {
	try {
		const j = await jget('/api/devices');
		const list = $('#deviceList');
		list.innerHTML = '';
		$('#deviceEmpty').classList.toggle('hidden', j.devices.length > 0);
		for (const d of j.devices) {
			const card = document.createElement('div');
			card.className = 'device-card';
			card.innerHTML = `
				${icon('monitor', 18)}
				<div class="d-info">
					<div class="d-name">${escapeHTML(d.name || '未命名设备')}</div>
					<div class="d-url">${escapeHTML(d.url)}</div>
				</div>
				<div class="d-actions"></div>`;
			const openBtn = document.createElement('a');
			openBtn.className = 'btn small primary';
			openBtn.textContent = '打开';
			openBtn.target = '_blank';
			openBtn.href = d.url;
			const copyBtn = document.createElement('button');
			copyBtn.className = 'btn small';
			copyBtn.textContent = '复制';
			copyBtn.onclick = async () => {
				const ok = await copyText(d.url);
				toast(ok ? '地址已复制' : '复制失败', ok ? 'ok' : 'err');
			};
			card.querySelector('.d-actions').appendChild(openBtn);
			card.querySelector('.d-actions').appendChild(copyBtn);
			list.appendChild(card);
		}
	} catch (_) { /* 静默 */ }
}

/* ================= 文本 ================= */
async function loadTexts() {
	try {
		const j = await jget('/api/texts');
		const list = $('#textList');
		list.innerHTML = '';
		if (!j.texts.length) {
			list.innerHTML = '<p class="muted">暂无文本，来发第一条吧。</p>';
			return;
		}
		for (const t of j.texts) {
			const item = document.createElement('div');
			item.className = 'text-item';
			const head = document.createElement('div');
			head.className = 't-head';
			head.innerHTML = `<span class="t-time">#${t.id} · ${fmtTime(t.time)}</span>`;
			const copy = document.createElement('button');
			copy.className = 't-btn';
			copy.textContent = '复制';
			copy.onclick = async () => {
				const ok = await copyText(t.content);
				toast(ok ? '已复制' : '复制失败', ok ? 'ok' : 'err');
			};
			const del = document.createElement('button');
			del.className = 't-btn';
			del.textContent = '删除';
			del.onclick = async () => {
				if (!await confirmBox(`删除文本 #${t.id}？`)) return;
				await jdel('/api/text/' + t.id);
				loadTexts();
			};
			head.appendChild(copy);
			head.appendChild(del);
			const body = document.createElement('div');
			body.className = 't-content';
			body.textContent = t.content;
			item.appendChild(head);
			item.appendChild(body);
			list.appendChild(item);
		}
	} catch (_) { /* 静默 */ }
}

async function sendText() {
	const ta = $('#textInput');
	const content = ta.value.trim();
	if (!content) { toast('内容不能为空', 'err'); return; }
	try {
		await jpost('/api/texts', { content });
		ta.value = '';
		toast('已发送');
		loadTexts();
	} catch (e) {
		toast(e.message, 'err');
	}
}
$('#btnSendText').onclick = sendText;
$('#textInput').addEventListener('keydown', (e) => {
	if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) sendText();
});
$('#btnPaste').onclick = async () => {
	const t = await pickClipboard();
	if (t == null) {
		toast('浏览器不允许读取剪贴板，请手动粘贴（Ctrl+V）', 'err');
		$('#textInput').focus();
		return;
	}
	$('#textInput').value = (t + '\n').trim();
};

/* ================= 历史 ================= */
const typeLabel = { send: '发出', receive: '接收', text: '分享文本', delete: '删除' };

async function loadHistory() {
	try {
		const j = await jget('/api/history');
		const tb = $('#historyBody');
		tb.innerHTML = '';
		$('#emptyHistory').classList.toggle('hidden', j.events.length > 0);
		for (const ev of j.events) {
			const tr = document.createElement('tr');
			tr.innerHTML = `
				<td>${fmtTime(ev.time)}</td>
				<td class="hf-ip">${escapeHTML(ev.peer || '—')}</td>
				<td><span class="tag ${ev.type}">${typeLabel[ev.type] || ev.type}</span></td>
				<td style="max-width:340px;word-break:break-all">${escapeHTML(ev.name)}</td>
				<td class="h-size">${fmtBytes(ev.size)}</td>`;
			tb.appendChild(tr);
		}
	} catch (e) {
		toast(e.message, 'err');
	}
}
$('#btnClearHistory').onclick = async () => {
	if (!await confirmBox('确定要清空所有传输记录吗？')) return;
	await jpost('/api/history/clear', {});
	loadHistory();
	toast('已清空');
};

/* ================= 初始化与轮询 ================= */
setInterval(() => {
	if (state.tab === 'devices') { loadDevices(); }
	if (state.tab === 'texts') { loadTexts(); }
}, 3000);

loadInfo();
navigate('');
loadDevices();
