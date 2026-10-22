/* =====================================================================
 * FileTra C 版 — 手写内嵌 HTTP 服务器（Winsock2），qrcodegen 仅用于二维码
 * 目标：i686 静态单 exe，WinXP SP3 ~ Win11 全兼容（32/64）
 * 协议与 Go 版逐字段对齐（前端 web/ 零改动直接复用）
 * ===================================================================== */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WINVER
#define WINVER 0x0501
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "qrcodegen.h"
#include "assets.h"

#define BEACON_BASE   52020
#define BEACON_PORTS  8
#define MAXHIST       500
#define MAXTEXTS      100
#define MAXTEXTSIZE   (200*1024)
#define MAX_BODY      (34*1024*1024)   /* 单请求体积上限（分片 4MB + 余量） */
#define TEMP_PREFIX_W L".ftpart-"
#define PWCAP         1060

/* ------------------------------------------------------------------
 * 基础：内存 / 动态池 / 字符串
 * ------------------------------------------------------------------ */
static void *xmalloc(size_t n){
	void *p = malloc(n ? n : 1);
	if (!p){ fprintf(stderr, "out of memory\n"); exit(1); }
	return p;
}

typedef struct { void **it; int n, cap; } Aren;
static void ar_add(Aren *a, void *p){
	if (a->n == a->cap){
		a->cap = a->cap ? a->cap*2 : 16;
		void **ni = (void**)realloc(a->it, sizeof(void*)*a->cap);
		if (!ni){ fprintf(stderr, "out of memory\n"); exit(1); }
		a->it = ni;
	}
	a->it[a->n++] = p;
}
static void *am(Aren *a, size_t n){ void *p = xmalloc(n); ar_add(a, p); return p; }
static char *ams(Aren *a, const char *s){
	size_t l = strlen(s);
	char *p = (char*)am(a, l+1);
	memcpy(p, s, l+1);
	return p;
}
static void ar_free(Aren *a){
	for (int i = 0; i < a->n; i++) free(a->it[i]);
	free(a->it);
	memset(a, 0, sizeof(*a));
}

typedef struct { char *p; size_t n, cap; } SB;
static void sb_grow(SB *s, size_t need){
	if (s->n + need + 1 > s->cap){
		size_t c = s->cap ? s->cap : 256;
		while (c < s->n + need + 1) c *= 2;
		char *q = (char*)realloc(s->p, c);
		if (!q){ fprintf(stderr, "out of memory\n"); exit(1); }
		s->p = q; s->cap = c;
	}
}
static void sb_add(SB *s, const void *d, size_t n){
	sb_grow(s, n);
	memcpy(s->p + s->n, d, n);
	s->n += n; s->p[s->n] = 0;
}
static void sb_str(SB *s, const char *t){ sb_add(s, t, strlen(t)); }
static void sb_ch(SB *s, char c){ sb_add(s, &c, 1); }
static void sb_u64(SB *s, unsigned long long u){
	char t[24]; int i = 0;
	do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
	while (i) sb_ch(s, t[--i]);
}
static void sb_i64(SB *s, long long v){
	if (v < 0){ sb_ch(s, '-'); sb_u64(s, (unsigned long long)(-(v + 1)) + 1ULL); }
	else sb_u64(s, (unsigned long long)v);
}
static void sb_w(SB *s, const wchar_t *w){ /* wide → utf8 追加 */
	int bl = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (bl > 0){
		char tmp[2048]; char *dp = tmp;
		if (bl > 2048) dp = (char*)xmalloc(bl + 2);
		WideCharToMultiByte(CP_UTF8, 0, w, -1, dp, bl, NULL, NULL);
		sb_add(s, dp, (size_t)bl - 1); /* 去掉 NUL */
		if (dp != tmp) free(dp);
	}
}

static wchar_t *u2w(Aren *a, const char *s){
	int wl = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = (wchar_t*)am(a, sizeof(wchar_t)*(wl + 2));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wl);
	return w;
}
static char *w2u(Aren *a, const wchar_t *w){
	int bl = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *b = (char*)am(a, bl + 2);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, b, bl, NULL, NULL);
	return b;
}
static char *w2u_m(const wchar_t *w){ /* 不经 arena，调用方 free */
	int bl = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *b = (char*)xmalloc(bl + 2);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, b, bl, NULL, NULL);
	return b;
}

/* ------------------------------------------------------------------
 * 全局配置
 * ------------------------------------------------------------------ */
static wchar_t g_rootW[PWCAP];        /* 共享目录绝对路径 */
static char    g_rootU8[PWCAP*2];
static wchar_t g_dataW[PWCAP];        /* 数据目录 %APPDATA%\filetra */
static char    g_nameU8[256];
static wchar_t g_nameW[256];
static char    g_pass[128];
static char    g_user[64] = "filetra";
static int     g_port = 5050;
static char    g_instance[24];        /* 12 hex */
static int     g_noBrowser = 0;
static SOCKET  g_listen = INVALID_SOCKET;

/* ------------------------------------------------------------------
 * 十六进制随机 / 常量比较 / base64
 * ------------------------------------------------------------------ */
static void randHex(char *out, int nbytes){
	unsigned char buf[16];
	HCRYPTPROV h = 0;
	int got = 0;
	memset(buf, 0, sizeof(buf));
	if (nbytes > (int)sizeof(buf)) nbytes = sizeof(buf);
	if (CryptAcquireContextW(&h, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)){
		if (CryptGenRandom(h, (DWORD)nbytes, buf)) got = 1;
		CryptReleaseContext(h, 0);
	}
	if (!got){
		unsigned seed = (unsigned)time(NULL) ^ GetTickCount();
		for (int i = 0; i < nbytes; i++){ seed = seed*1664525u + 1013904223u; buf[i] = (unsigned char)(seed >> 16); }
	}
	for (int i = 0; i < nbytes; i++)
		sprintf(out + 2*i, "%02x", buf[i]);
	out[2*nbytes] = 0;
}
static int consteq(const char *a, const char *b){
	size_t la = strlen(a), lb = strlen(b), m = la > lb ? la : lb;
	unsigned char d = (unsigned char)(la ^ lb);
	for (size_t i = 0; i < m; i++){
		unsigned char ca = (unsigned char)(i < la ? a[i] : 0);
		unsigned char cb = (unsigned char)(i < lb ? b[i] : 0);
		d |= (unsigned char)(ca ^ cb);
	}
	return d == 0;
}
static int hexv(char c){
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
static int b64_dec(const char *in, char *out, int outMax){
	static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int val = 0, bits = 0, n = 0;
	for (const char *p = in; *p; p++){
		char c = *p;
		if (c == '=') break;
		if (c == '\r' || c == '\n' || c == ' ') continue;
		const char *q = strchr(T, c);
		if (!q) return -1;
		val = (val << 6) | (int)(q - T);
		bits += 6;
		if (bits >= 8){
			bits -= 8;
			if (n < outMax) out[n++] = (char)((val >> bits) & 0xFF);
		}
	}
	return n;
}

/* ------------------------------------------------------------------
 * URL 解码（path 段 / query 值）
 * ------------------------------------------------------------------ */
static char *url_dec(Aren *a, const char *s, size_t n, int plusAsSpace){
	char *o = (char*)am(a, n + 3);
	size_t i = 0, j = 0;
	while (i < n){
		char c = s[i];
		if (c == '%' && i + 2 < n && hexv(s[i+1]) >= 0 && hexv(s[i+2]) >= 0){
			o[j++] = (char)(hexv(s[i+1]) * 16 + hexv(s[i+2]));
			i += 3;
		} else if (plusAsSpace && c == '+'){
			o[j++] = ' '; i++;
		} else {
			o[j++] = c; i++;
		}
	}
	o[j] = 0;
	return o;
}

/* ------------------------------------------------------------------
 * 极简 JSON：解析（arena）+ 写出
 * ------------------------------------------------------------------ */
typedef struct JV JV;
struct JV {
	int type;          /* 0 null 1 bool 2 int 3 str 4 arr 5 obj */
	int b;
	long long inum;
	char *str;
	JV **kids;
	char **keys;
	int nk;
};
typedef struct { const char *p; Aren *a; int ok; } JP;

static JV *jv_new(JP *j, int t){
	JV *v = (JV*)am(j->a, sizeof(JV));
	memset(v, 0, sizeof(JV));
	v->type = t;
	return v;
}
static void jskip(JP *j){
	while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n') j->p++;
}
static void utf8_pt(SB *s, unsigned u){
	if (u < 0x80) sb_ch(s, (char)u);
	else if (u < 0x800){ sb_ch(s, (char)(0xC0 | (u >> 6))); sb_ch(s, (char)(0x80 | (u & 63))); }
	else if (u < 0x10000){ sb_ch(s, (char)(0xE0 | (u >> 12))); sb_ch(s, (char)(0x80 | ((u >> 6) & 63))); sb_ch(s, (char)(0x80 | (u & 63))); }
	else { sb_ch(s, (char)(0xF0 | (u >> 18))); sb_ch(s, (char)(0x80 | ((u >> 12) & 63))); sb_ch(s, (char)(0x80 | ((u >> 6) & 63))); sb_ch(s, (char)(0x80 | (u & 63))); }
}
static char *jstr_rd(JP *j){
	SB sb; memset(&sb, 0, sizeof(sb));
	if (*j->p != '"'){ j->ok = 0; return NULL; }
	j->p++;
	for (;;){
		unsigned char c = (unsigned char)*j->p;
		if (!c){ j->ok = 0; free(sb.p); return NULL; }
		if (c == '"'){ j->p++; break; }
		if (c == '\\'){
			j->p++;
			char e = *j->p++;
			switch (e){
				case '"':  sb_ch(&sb, '"');  break;
				case '\\': sb_ch(&sb, '\\'); break;
				case '/':  sb_ch(&sb, '/');  break;
				case 'n':  sb_ch(&sb, '\n'); break;
				case 't':  sb_ch(&sb, '\t'); break;
				case 'r':  sb_ch(&sb, '\r'); break;
				case 'b':  sb_ch(&sb, '\b'); break;
				case 'f':  sb_ch(&sb, '\f'); break;
				case 'u': {
					unsigned u = 0;
					for (int k = 0; k < 4; k++){
						int h = hexv(*j->p++);
						if (h < 0){ j->ok = 0; free(sb.p); return NULL; }
						u = u * 16 + (unsigned)h;
					}
					if (u >= 0xD800 && u < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u'){
						const char *sv = j->p;
						j->p += 2;
						unsigned lo = 0; int bad = 0;
						for (int k = 0; k < 4; k++){
							int h = hexv(j->p[k]);
							if (h < 0){ bad = 1; break; }
							lo = lo * 16 + (unsigned)h;
						}
						if (!bad && lo >= 0xDC00 && lo < 0xE000){
							j->p += 4;
							u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
						} else j->p = sv;
					}
					utf8_pt(&sb, u);
				} break;
				default: j->ok = 0; free(sb.p); return NULL;
			}
			if (!j->ok && sb.p) { free(sb.p); return NULL; }
		} else {
			sb_ch(&sb, (char)c);
			j->p++;
		}
	}
	char *out = (char*)am(j->a, sb.n + 1);
	if (sb.n) memcpy(out, sb.p, sb.n);
	out[sb.n] = 0;
	free(sb.p);
	return out;
}
static long long j_atoll(const char *s, const char **end){
	long long v = 0; int neg = 0;
	if (*s == '-'){ neg = 1; s++; } else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9'){
		if (v < 0x1999999999999999LL) v = v*10 + (*s - '0');
		s++;
	}
	if (end) *end = s;
	return neg ? -v : v;
}
static JV *jval(JP *j){
	char **ks = NULL; JV **vs = NULL;
	jskip(j);
	if (*j->p == '{'){
		int n = 0, c = 0;
		j->p++;
		for (;;){
			jskip(j);
			if (*j->p == '}'){ j->p++; break; }
			char *k = jstr_rd(j);
			if (!k || !j->ok) goto fail;
			jskip(j);
			if (*j->p != ':') goto fail;
			j->p++;
			JV *val = jval(j);
			if (!j->ok || !val) goto fail;
			if (n == c){
				c = c ? c*2 : 8;
				ks = (char**)realloc(ks, sizeof(char*)*c);
				vs = (JV**)realloc(vs, sizeof(JV*)*c);
				if (!ks || !vs) goto fail;
			}
			ks[n] = k; vs[n] = val; n++;
			jskip(j);
			if (*j->p == ','){ j->p++; continue; }
			if (*j->p == '}'){ j->p++; break; }
			goto fail;
		}
		JV *v = jv_new(j, 5);
		v->nk = n;
		v->keys = (char**)am(j->a, sizeof(char*)*(n ? n : 1));
		v->kids = (JV**)am(j->a, sizeof(JV*)*(n ? n : 1));
		if (n){ memcpy(v->keys, ks, sizeof(char*)*n); memcpy(v->kids, vs, sizeof(JV*)*n); }
		free(ks); free(vs);
		return v;
	}
	if (*j->p == '['){
		int n = 0, c = 0;
		j->p++;
		for (;;){
			jskip(j);
			if (*j->p == ']'){ j->p++; break; }
			JV *val = jval(j);
			if (!j->ok || !val) goto fail;
			if (n == c){
				c = c ? c*2 : 8;
				vs = (JV**)realloc(vs, sizeof(JV*)*c);
				if (!vs) goto fail;
			}
			vs[n++] = val;
			jskip(j);
			if (*j->p == ','){ j->p++; continue; }
			if (*j->p == ']'){ j->p++; break; }
			goto fail;
		}
		JV *v = jv_new(j, 4);
		v->nk = n;
		v->kids = (JV**)am(j->a, sizeof(JV*)*(n ? n : 1));
		if (n) memcpy(v->kids, vs, sizeof(JV*)*n);
		free(vs);
		return v;
	}
	if (*j->p == '"'){
		JV *v = jv_new(j, 3);
		v->str = jstr_rd(j);
		if (!v->str) return NULL;
		return v;
	}
	if (*j->p == 't'){ if (!strncmp(j->p, "true", 4)){ j->p += 4; JV *v = jv_new(j, 1); v->b = 1; return v; } j->ok = 0; return NULL; }
	if (*j->p == 'f'){ if (!strncmp(j->p, "false", 5)){ j->p += 5; return jv_new(j, 1); } j->ok = 0; return NULL; }
	if (*j->p == 'n'){ if (!strncmp(j->p, "null", 4)){ j->p += 4; return jv_new(j, 0); } j->ok = 0; return NULL; }
	if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')){
		long long v = j_atoll(j->p, &j->p);
		if (*j->p == '.' || *j->p == 'e' || *j->p == 'E'){ /* 深度略过小数部分（本项目全部为整数） */
			while ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '-' || *j->p == '+') j->p++;
		}
		JV *o = jv_new(j, 2);
		o->inum = v;
		return o;
	}
	j->ok = 0;
	return NULL;
fail:
	if (ks) free(ks);
	if (vs) free(vs);
	j->ok = 0;
	return NULL;
}
static JV *jparse(Aren *a, const char *s){
	JP j; j.p = s; j.a = a; j.ok = 1;
	JV *v = jval(&j);
	if (!j.ok) return NULL;
	return v;
}
static JV *jget(JV *o, const char *k){
	if (!o || o->type != 5) return NULL;
	for (int i = 0; i < o->nk; i++)
		if (!strcmp(o->keys[i], k)) return o->kids[i];
	return NULL;
}
static const char *jstr_get(JV *o, const char *k, const char *dft){
	JV *v = jget(o, k);
	return (v && v->type == 3) ? v->str : dft;
}
static long long jnum_get(JV *o, const char *k, long long dft){
	JV *v = jget(o, k);
	return (v && v->type == 2) ? v->inum : dft;
}

static void json_str(SB *s, const char *t){
	sb_ch(s, '"');
	for (const unsigned char *p = (const unsigned char*)t; *p; p++){
		unsigned char c = *p;
		switch (c){
			case '"':  sb_str(s, "\\\""); break;
			case '\\': sb_str(s, "\\\\"); break;
			case '\n': sb_str(s, "\\n");  break;
			case '\r': sb_str(s, "\\r");  break;
			case '\t': sb_str(s, "\\t");  break;
			case '\b': sb_str(s, "\\b");  break;
			case '\f': sb_str(s, "\\f");  break;
			default:
				if (c < 0x20){
					char h[8];
					sprintf(h, "\\u%04x", c);
					sb_str(s, h);
				} else sb_ch(s, (char)c);
		}
	}
	sb_ch(s, '"');
}

/* ------------------------------------------------------------------
 * 文件操作（全部 Win32 W API，避免 CRT 大文件/编码坑）
 * ------------------------------------------------------------------ */
typedef struct { long long size, mtime; int isDir; } FINFO;
static int wstat(const wchar_t *path, FINFO *fi){
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(path, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	FindClose(h);
	fi->size = (long long)(((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
	ULONGLONG ft = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
	fi->mtime = ft < 116444736000000000ULL ? 0 : (long long)((ft - 116444736000000000ULL) / 10000000ULL);
	fi->isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	return 1;
}
static int wccat(wchar_t *dst, size_t cap, const wchar_t *src){
	size_t l = wcslen(dst), m = wcslen(src);
	if (l + m + 1 >= cap) return 0;
	wcscpy(dst + l, src);
	return 1;
}
static void rmrfW(const wchar_t *path, int depth){
	if (depth > 60) return;
	wchar_t pat[PWCAP];
	wcscpy(pat, path);
	if (!wccat(pat, PWCAP, L"\\*")) return;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		wchar_t sub[PWCAP];
		wcscpy(sub, path);
		if (!wccat(sub, PWCAP, L"\\")) continue;
		if (!wccat(sub, PWCAP, fd.cFileName)) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmrfW(sub, depth + 1);
		else DeleteFileW(sub);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	RemoveDirectoryW(path);
}
static void wnum(wchar_t *dst, size_t cap, long long v){
	char t[24]; int i = 0, neg = 0;
	unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
	do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (neg) t[i++] = '-';
	dst[0] = 0;
	while (i){
		if (wcslen(dst) + 1 >= cap) return;
		wchar_t c = (wchar_t)t[--i];
		size_t l = wcslen(dst);
		dst[l] = c; dst[l+1] = 0;
	}
}
static void uniqueNameW(Aren *a, const wchar_t *dir, const char *nameU8, wchar_t *out){
	wchar_t *nw = u2w(a, nameU8);
	wcscpy(out, dir);
	if (!wccat(out, PWCAP, L"\\")) return;
	if (!wccat(out, PWCAP, nw)) return;
	FINFO fi;
	if (!wstat(out, &fi)) return; /* 不存在 → 直接用 */
	/* 分离扩展名 */
	size_t bl = wcslen(nw);
	for (size_t i = bl; i > 0; i--){
		if (nw[i-1] == L'.'){ bl = i - 1; break; }
		if (i == 1){ bl = wcslen(nw); } /* 无扩展名 */
	}
	wchar_t base[520], ext[260];
	if (bl > 519) bl = 519;
	memcpy(base, nw, bl * sizeof(wchar_t)); base[bl] = 0;
	if (bl < wcslen(nw)) wcscpy(ext, nw + bl); else ext[0] = 0;
	for (long long i = 1; ; i++){
		wchar_t cand[PWCAP], num[24];
		wcscpy(cand, dir);
		if (!wccat(cand, PWCAP, L"\\")) return;
		if (!wccat(cand, PWCAP, base)) return;
		if (!wccat(cand, PWCAP, L" (")) return;
		wnum(num, 24, i);
		if (!wccat(cand, PWCAP, num)) return;
		if (!wccat(cand, PWCAP, L")")) return;
		if (!wccat(cand, PWCAP, ext)) return;
		if (!wstat(cand, &fi)){ wcscpy(out, cand); return; }
	}
}
static int copyFileW(const wchar_t *src, const wchar_t *dst){
	HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (in == INVALID_HANDLE_VALUE) return 0;
	HANDLE out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (out == INVALID_HANDLE_VALUE){ CloseHandle(in); return 0; }
	static const DWORD BZ = 1 << 20;
	char *buf = (char*)xmalloc(BZ);
	int ok = 1;
	for (;;){
		DWORD got = 0;
		if (!ReadFile(in, buf, BZ, &got, NULL)){ ok = 0; break; }
		if (!got) break;
		DWORD done = 0;
		while (done < got){
			DWORD w = 0;
			if (!WriteFile(out, buf + done, got - done, &w, NULL) || !w){ ok = 0; break; }
			done += w;
		}
		if (!ok) break;
	}
	FlushFileBuffers(out);
	free(buf);
	CloseHandle(in); CloseHandle(out);
	return ok;
}
static int mkdirSubW(const wchar_t *root, wchar_t *segs[], int n){
	wchar_t cur[PWCAP];
	wcscpy(cur, root);
	for (int i = 0; i < n; i++){
		if (!wccat(cur, PWCAP, L"\\")) return 0;
		if (!wccat(cur, PWCAP, segs[i])) return 0;
		if (!CreateDirectoryW(cur, NULL)){
			DWORD e = GetLastError();
			if (e != ERROR_ALREADY_EXISTS) return 0;
		}
		FINFO fi;
		if (!wstat(cur, &fi) || !fi.isDir) return 0;
	}
	return 1;
}
static int writeFileW(const wchar_t *path, const void *data, size_t n){
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return 0;
	int ok = 1;
	const char *p = (const char*)data;
	size_t off = 0;
	while (off < n){
		DWORD w = 0;
		if (!WriteFile(h, p + off, (DWORD)((n - off) > 0x40000000 ? 0x40000000 : (n - off)), &w, NULL) || !w){ ok = 0; break; }
		off += w;
	}
	CloseHandle(h);
	return ok;
}
static char *readFileW(Aren *a, const wchar_t *path, size_t *outLen){
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return NULL;
	LARGE_INTEGER sz;
	if (!GetFileSizeEx(h, &sz)){ CloseHandle(h); return NULL; }
	long long n = sz.QuadPart;
	if (n > 32 * 1024 * 1024){ CloseHandle(h); return NULL; }
	char *b = (char*)am(a, (size_t)n + 1);
	DWORD got = 0;
	int ok = 1;
	size_t off = 0;
	while ((long long)off < n){
		if (!ReadFile(h, b + off, (DWORD)(n - off), &got, NULL)){ ok = 0; break; }
		if (!got) break;
		off += got;
	}
	CloseHandle(h);
	if (!ok) return NULL;
	b[off] = 0;
	if (outLen) *outLen = off;
	return b;
}

/* ------------------------------------------------------------------
 * CRC32 / zip（STORE 流式）
 * ------------------------------------------------------------------ */
static unsigned crc32_tab[256];
static int crc32_ready = 0;
static void crc32_init(void){
	for (unsigned i = 0; i < 256; i++){
		unsigned c = i;
		for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc32_tab[i] = c;
	}
	crc32_ready = 1;
}
static unsigned crc32_add(unsigned c, const unsigned char *p, size_t n){
	while (n--) c = crc32_tab[(c ^ (*p++)) & 0xFF] ^ (c >> 8);
	return c;
}
static void dosTime(const FILETIME *ft, unsigned *date, unsigned *tim){
	FILETIME lt;
	SYSTEMTIME st;
	if (!FileTimeToLocalFileTime(ft, &lt) || !FileTimeToSystemTime(&lt, &st)){
		*date = 0; *tim = 0; return;
	}
	if (st.wYear < 1980){ *date = 0x0021; *tim = 0; return; }
	*date = (unsigned)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
	*tim  = (unsigned)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}

typedef struct {
	wchar_t abs[PWCAP];
	char rel[560];
	unsigned long long size;
	FILETIME ft;
} ZWItem;

static void zipWalk(const wchar_t *dir, const char *pref, Aren *a, ZWItem **arr, int *n, int *cap, int depth){
	if (depth > 40) return;
	wchar_t pat[PWCAP];
	wcscpy(pat, dir);
	if (!wccat(pat, PWCAP, L"\\*")) return;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		if (fd.cFileName[0] == L'.') continue;                 /* 与 Go 版一致：隐藏/临时文件跳过 */
		if (!wcsncmp(fd.cFileName, TEMP_PREFIX_W, 8)) continue;
		wchar_t sub[PWCAP];
		wcscpy(sub, dir);
		if (!wccat(sub, PWCAP, L"\\")) continue;
		if (!wccat(sub, PWCAP, fd.cFileName)) continue;
		int bl = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
		if (bl < 1) continue;
		char *seg = (char*)am(a, bl + 2);
		WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, seg, bl, NULL, NULL);
		char rel[600];
		if (pref[0]){ if (strlen(pref) + 1 + (size_t)bl + 1 >= sizeof(rel)) continue; sprintf(rel, "%s/%s", pref, seg); }
		else { if ((size_t)bl + 1 >= sizeof(rel)) continue; strcpy(rel, seg); }
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
			zipWalk(sub, rel, a, arr, n, cap, depth + 1);
		} else {
			if (strlen(rel) >= 560) continue;
			if (*n == *cap){
				*cap = *cap ? (*cap)*2 : 32;
				ZWItem *na2 = (ZWItem*)realloc(*arr, sizeof(ZWItem)*(*cap));
				if (!na2) continue;
				*arr = na2;
			}
			ZWItem *it = &(*arr)[(*n)++];
			wcscpy(it->abs, sub);
			strcpy(it->rel, rel);
			it->size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			it->ft = fd.ftLastWriteTime;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static int sock_sendall(SOCKET s, const char *b, size_t n){
	while (n){
		int w = send(s, b, (int)(n > (1u<<20) ? (1u<<20) : n), 0);
		if (w <= 0){
			int e = WSAGetLastError();
			if (e == WSAEWOULDBLOCK || e == WSAEINTR){ Sleep(5); continue; }
			return 0;
		}
		b += w; n -= (size_t)w;
	}
	return 1;
}

static void zi_w16(unsigned char *t, unsigned v){ t[0] = v & 0xFF; t[1] = (v >> 8) & 0xFF; }
static void zi_w32(unsigned char *t, unsigned long v){ t[0] = v & 0xFF; t[1] = (v >> 8) & 0xFF; t[2] = (v >> 16) & 0xFF; t[3] = (v >> 24) & 0xFF; }

/* ------------------------------------------------------------------
 * QR 码 + 极简 PNG（zlib stored 块）
 * ------------------------------------------------------------------ */
static unsigned png_crc32(const unsigned char *p, size_t n){
	unsigned c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = crc32_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}
static void png_w32be(unsigned char *t, unsigned long v){ /* PNG 大端 */
	t[0] = (unsigned char)((v >> 24) & 0xFF);
	t[1] = (unsigned char)((v >> 16) & 0xFF);
	t[2] = (unsigned char)((v >> 8) & 0xFF);
	t[3] = (unsigned char)(v & 0xFF);
}
static unsigned adler32(const unsigned char *p, size_t n){
	unsigned a = 1, b = 0;
	for (size_t i = 0; i < n; i++){ a = (a + p[i]) % 65521; b = (b + a) % 65521; }
	return (b << 16) | a;
}
static void png_chunk(SB *s, const char *type, const unsigned char *data, unsigned len){
	unsigned char hdr[8];
	png_w32be(hdr, len);
	memcpy(hdr + 4, type, 4);
	sb_add(s, hdr, 8);
	if (len) sb_add(s, data, len);
	unsigned char *buf = (unsigned char*)xmalloc(len + 4);
	memcpy(buf, type, 4);              /* PNG 规范：CRC 先 type 后 data */
	if (len) memcpy(buf + 4, data, len);
	unsigned crc = png_crc32(buf, len + 4);
	unsigned char c4[4];
	png_w32be(c4, crc);
	sb_add(s, c4, 4);
	free(buf);
}
static int qr_png(SB *out, const char *text){
	uint8_t q[qrcodegen_BUFFER_LEN_MAX];
	uint8_t t[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(text, t, q, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
		return 0;
	int n = qrcodegen_getSize(q);
	int W = 320;
	int mod = n + 8;
	int scale = W / mod; if (scale < 1) scale = 1;
	int used = mod * scale;
	int pad = (W - used) / 2; if (pad < 0) pad = 0;
	size_t rawN = (size_t)W * ((size_t)W + 1);
	unsigned char *raw = (unsigned char*)xmalloc(rawN);
	size_t ro = 0;
	for (int y = 0; y < W; y++){
		raw[ro++] = 0; /* filter none */
		int qy = (y - pad) / scale;
		for (int x = 0; x < W; x++){
			int qx = (x - pad) / scale;
			int on = (qx >= 0 && qy >= 0 && qx < n && qy < n && qrcodegen_getModule(q, qx, qy));
			raw[ro++] = on ? 0 : 255;
		}
	}
	/* zlib 包裹的 stored deflate 块 */
	SB idat; memset(&idat, 0, sizeof(idat));
	sb_add(&idat, "\x78\x01", 2);
	size_t off = 0;
	while (off < rawN){
		size_t blk = rawN - off;
		if (blk > 65535) blk = 65535;
		unsigned char h[5];
		h[0] = (off + blk >= rawN) ? 1 : 0;
		zi_w16(h + 1, (unsigned)blk);
		zi_w16(h + 3, (unsigned)(~blk & 0xFFFF));
		sb_add(&idat, h, 5);
		sb_add(&idat, raw + off, blk);
		off += blk;
	}
	unsigned ad = adler32(raw, rawN);
	unsigned char tail[4];
	tail[0] = (unsigned char)(ad >> 24); tail[1] = (unsigned char)(ad >> 16);
	tail[2] = (unsigned char)(ad >> 8);  tail[3] = (unsigned char)ad;
	sb_add(&idat, tail, 4);
	free(raw);

	out->n = 0;
	sb_add(out, "\x89PNG\r\n\x1a\n", 8);
	unsigned char ihdr[13];
	png_w32be(ihdr, (unsigned long)W);
	png_w32be(ihdr + 4, (unsigned long)W);
	ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	png_chunk(out, "IHDR", ihdr, 13);
	png_chunk(out, "IDAT", (const unsigned char*)idat.p, (unsigned)idat.n);
	png_chunk(out, "IEND", NULL, 0);
	free(idat.p);
	return 1;
}
static void put16(unsigned w, char *o){ static const char *HX = "0123456789ABCDEF"; o[0] = HX[(w >> 4) & 15]; o[1] = HX[w & 15]; }
static void pctenc(SB *s, const char *t){
	for (const unsigned char *p = (const unsigned char*)t; *p; p++){
		if (*p <= 0x20 || *p >= 0x7F || *p == '"' || *p == '%' || *p == '\\'){
			char h[3]; put16(*p, h); sb_ch(s, '%'); sb_add(s, h, 2);
		} else sb_ch(s, (char)*p);
	}
}

/* ------------------------------------------------------------------
 * 持久化：历史 / 文本
 * ------------------------------------------------------------------ */
typedef struct { char type[8]; char *name; char *peer; long long time; long long size; } Ev;
typedef struct { char *content; long long id, time; } Tx;
static Ev  g_hist[MAXHIST]; static int g_hn = 0;
static Tx  g_texts[256];    static int g_tn = 0;
static long long g_nextText = 1;
static CRITICAL_SECTION g_stLock;
static wchar_t g_histPathW[PWCAP], g_textsPathW[PWCAP];

static void store_freeEv(Ev *e){ free(e->name); free(e->peer); e->name = e->peer = NULL; }
static void store_freeTx(Tx *t){ free(t->content); t->content = NULL; }

static void store_marshallEvents(SB *s){
	sb_ch(s, '[');
	for (int i = 0; i < g_hn; i++){
		if (i) sb_ch(s, ',');
		sb_str(s, "{\"time\":"); sb_i64(s, g_hist[i].time);
		sb_str(s, ",\"type\":"); json_str(s, g_hist[i].type);
		sb_str(s, ",\"name\":"); json_str(s, g_hist[i].name);
		sb_str(s, ",\"size\":"); sb_i64(s, g_hist[i].size);
		sb_str(s, ",\"peer\":"); json_str(s, g_hist[i].peer);
		sb_ch(s, '}');
	}
	sb_ch(s, ']');
}
static void store_marshallEventsRev(SB *s){
	sb_ch(s, '[');
	for (int i = g_hn - 1; i >= 0; i--){
		if (i != g_hn - 1) sb_ch(s, ',');
		sb_str(s, "{\"time\":"); sb_i64(s, g_hist[i].time);
		sb_str(s, ",\"type\":"); json_str(s, g_hist[i].type);
		sb_str(s, ",\"name\":"); json_str(s, g_hist[i].name);
		sb_str(s, ",\"size\":"); sb_i64(s, g_hist[i].size);
		sb_str(s, ",\"peer\":"); json_str(s, g_hist[i].peer);
		sb_ch(s, '}');
	}
	sb_ch(s, ']');
}
static void store_marshallTexts(SB *s){
	sb_ch(s, '[');
	for (int i = 0; i < g_tn; i++){
		if (i) sb_ch(s, ',');
		sb_str(s, "{\"id\":"); sb_i64(s, g_texts[i].id);
		sb_str(s, ",\"content\":"); json_str(s, g_texts[i].content);
		sb_str(s, ",\"time\":"); sb_i64(s, g_texts[i].time);
		sb_ch(s, '}');
	}
	sb_ch(s, ']');
}
static int store_save(const wchar_t *path, const char *data, size_t n){
	wchar_t tmp[PWCAP];
	wcscpy(tmp, path);
	if (!wccat(tmp, PWCAP, L".tmp")) return 0;
	if (!writeFileW(tmp, data, n)) return 0;
	if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 0;
	return 1;
}
static void store_loadAll(void){
	InitializeCriticalSection(&g_stLock);
	Aren a; memset(&a, 0, sizeof(a));
	size_t n = 0;
	char *hb = readFileW(&a, g_histPathW, &n);
	if (hb){
		JV *root = jparse(&a, hb);
		if (root && root->type == 4){
			for (int i = 0; i < root->nk && g_hn < MAXHIST; i++){
				JV *e = root->kids[i];
				if (!e || e->type != 5) continue;
				Ev *ev = &g_hist[g_hn++];
				memset(ev, 0, sizeof(Ev));
				strncpy(ev->type, jstr_get(e, "type", "send"), 7);
				ev->type[7] = 0;
				ev->name = _strdup(jstr_get(e, "name", ""));
				ev->peer = _strdup(jstr_get(e, "peer", ""));
				ev->time = jnum_get(e, "time", 0);
				ev->size = jnum_get(e, "size", 0);
				if (!ev->name) ev->name = _strdup("");
				if (!ev->peer) ev->peer = _strdup("");
			}
		}
	}
	char *tb = readFileW(&a, g_textsPathW, &n);
	if (tb){
		JV *root = jparse(&a, tb);
		if (root && root->type == 4){
			for (int i = 0; i < root->nk && g_tn < 200; i++){
				JV *e = root->kids[i];
				if (!e || e->type != 5) continue;
				Tx *t = &g_texts[g_tn++];
				memset(t, 0, sizeof(Tx));
				t->id = jnum_get(e, "id", 0);
				t->content = _strdup(jstr_get(e, "content", ""));
				t->time = jnum_get(e, "time", 0);
				if (!t->content) t->content = _strdup("");
				if (t->id >= g_nextText) g_nextText = t->id + 1;
			}
		}
	}
	ar_free(&a);
}
static void store_addEvent(const char *type, const char *name, long long size, const char *peer){
	EnterCriticalSection(&g_stLock);
	Ev *e;
	if (g_hn >= MAXHIST){
		store_freeEv(&g_hist[0]);
		memmove(&g_hist[0], &g_hist[1], sizeof(Ev)*(MAXHIST - 1));
		e = &g_hist[MAXHIST - 1];
	} else e = &g_hist[g_hn++];
	memset(e, 0, sizeof(Ev));
	strncpy(e->type, type, 7);
	e->name = _strdup(name ? name : "");
	e->peer = _strdup(peer ? peer : "");
	e->time = (long long)time(NULL);
	e->size = size;
	SB sb; memset(&sb, 0, sizeof(sb));
	store_marshallEvents(&sb);
	store_save(g_histPathW, sb.p, sb.n);
	free(sb.p);
	LeaveCriticalSection(&g_stLock);
}
static void store_clearEvents(void){
	EnterCriticalSection(&g_stLock);
	for (int i = 0; i < g_hn; i++) store_freeEv(&g_hist[i]);
	g_hn = 0;
	SB sb; memset(&sb, 0, sizeof(sb));
	store_marshallEvents(&sb);
	store_save(g_histPathW, sb.p, sb.n);
	free(sb.p);
	LeaveCriticalSection(&g_stLock);
}
static Tx *store_addText(const char *content){
	EnterCriticalSection(&g_stLock);
	Tx *t;
	if (g_tn >= MAXTEXTS){
		store_freeTx(&g_texts[0]);
		memmove(&g_texts[0], &g_texts[1], sizeof(Tx)*(MAXTEXTS - 1));
		t = &g_texts[MAXTEXTS - 1];
	} else t = &g_texts[g_tn++];
	memset(t, 0, sizeof(Tx));
	t->id = g_nextText++;
	t->content = _strdup(content);
	t->time = (long long)time(NULL);
	SB sb; memset(&sb, 0, sizeof(sb));
	store_marshallTexts(&sb);
	store_save(g_textsPathW, sb.p, sb.n);
	free(sb.p);
	LeaveCriticalSection(&g_stLock);
	return t;
}
static void store_delText(long long id){
	EnterCriticalSection(&g_stLock);
	for (int i = 0; i < g_tn; i++){
		if (g_texts[i].id == id){
			store_freeTx(&g_texts[i]);
			memmove(&g_texts[i], &g_texts[i+1], sizeof(Tx)*(g_tn - 1 - i));
			g_tn--;
			break;
		}
	}
	SB sb; memset(&sb, 0, sizeof(sb));
	store_marshallTexts(&sb);
	store_save(g_textsPathW, sb.p, sb.n);
	free(sb.p);
	LeaveCriticalSection(&g_stLock);
}
static void store_clearTexts(void){
	EnterCriticalSection(&g_stLock);
	for (int i = 0; i < g_tn; i++) store_freeTx(&g_texts[i]);
	g_tn = 0;
	SB sb; memset(&sb, 0, sizeof(sb));
	store_marshallTexts(&sb);
	store_save(g_textsPathW, sb.p, sb.n);
	free(sb.p);
	LeaveCriticalSection(&g_stLock);
}
static void utf_trim(const char **s, size_t *n){
	const char *p = *s; size_t len = *n;
	while (len && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')){ p++; len--; }
	while (len && (p[len-1] == ' ' || p[len-1] == '\t' || p[len-1] == '\r' || p[len-1] == '\n')) len--;
	*s = p; *n = len;
}

/* ------------------------------------------------------------------
 * 上传会话（分片 / 断点续传）
 * ------------------------------------------------------------------ */
typedef struct Sess {
	struct Sess *next;
	char id[33];
	wchar_t tmpW[PWCAP];
	char *dirRel;      /* utf8，"a/b" 或 "" */
	char *display;     /* utf8 */
	CRITICAL_SECTION cs;
	long long size, received;
	time_t last;
	int done;
} Sess;
static Sess *g_sess = NULL;
static CRITICAL_SECTION g_sesLock;

static void sess_add(Sess *s){
	EnterCriticalSection(&g_sesLock);
	s->next = g_sess;
	g_sess = s;
	LeaveCriticalSection(&g_sesLock);
}
static void sess_remove_locked(Sess *s){
	Sess **pp = &g_sess;
	while (*pp && *pp != s) pp = &(*pp)->next;
	if (!*pp) return;
	*pp = s->next;
	/* 确保无人再持有 cs（进入后立即退出），随后方可安全销毁 */
	EnterCriticalSection(&s->cs);
	LeaveCriticalSection(&s->cs);
	DeleteCriticalSection(&s->cs);
	DeleteFileW(s->tmpW);
	free(s->dirRel);
	free(s->display);
	free(s);
}
static Sess *sess_get(const char *id){
	EnterCriticalSection(&g_sesLock);
	Sess *s = g_sess;
	while (s && strcmp(s->id, id)) s = s->next;
	LeaveCriticalSection(&g_sesLock);
	return s;
}
static int hex16ok(const char *s){
	if (strlen(s) != 16) return 0;
	for (int i = 0; i < 16; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
	return 1;
}
static Sess *sess_find(Aren *a, const char *id, int *code404){
	if (!hex16ok(id)){ *code404 = 1; return NULL; }
	Sess *s = sess_get(id);
	if (!s) *code404 = 2;
	return s;
}
static DWORD WINAPI janitorProc(LPVOID u){
	for (;;){
		Sleep(10 * 60 * 1000);
		time_t now = time(NULL);
		EnterCriticalSection(&g_sesLock);
		Sess *s = g_sess;
		while (s){
			Sess *nx = s->next;
			BOOL stale;
			EnterCriticalSection(&s->cs);
			stale = (now - s->last) > 2*3600;
			LeaveCriticalSection(&s->cs);
			if (stale) sess_remove_locked(s);
			s = nx;
		}
		LeaveCriticalSection(&g_sesLock);
	}
	return 0;
}

/* import：设备发现需用到 name/url 的 JSON */
static void dev_upsert(const char *id, const char *name, const char *url, const char *ip);

/* ------------------------------------------------------------------
 * 设备发现（UDP 广播）
 * ------------------------------------------------------------------ */
typedef struct Dev {
	struct Dev *next;
	char id[64], nameU8[256], url[256], ip[64];
	time_t seen;
} Dev;
static Dev *g_devs = NULL;
static CRITICAL_SECTION g_devLock;

static void dev_upsert(const char *id, const char *name, const char *url, const char *ip){
	if (!id || !id[0]) return;
	if (!strcmp(id, g_instance)) return;
	EnterCriticalSection(&g_devLock);
	Dev *d = g_devs;
	while (d && strcmp(d->id, id)) d = d->next;
	if (!d){
		d = (Dev*)xmalloc(sizeof(Dev));
		memset(d, 0, sizeof(Dev));
		d->next = g_devs;
		g_devs = d;
		strncpy(d->id, id, 63);
	}
	if (name) strncpy(d->nameU8, name, 255);
	if (url)  strncpy(d->url, url, 255);
	if (ip)   strncpy(d->ip, ip, 63);
	d->nameU8[255] = 0; d->url[255] = 0; d->ip[63] = 0;
	d->seen = time(NULL);
	LeaveCriticalSection(&g_devLock);
}
static void dev_prune(void){
	time_t now = time(NULL);
	EnterCriticalSection(&g_devLock);
	Dev **pp = &g_devs;
	while (*pp){
		if (now - (*pp)->seen > 12){
			Dev *d = *pp; *pp = d->next; free(d);
		} else pp = &(*pp)->next;
	}
	LeaveCriticalSection(&g_devLock);
}
/* 网卡地址表（含掩码）：GetIpAddrTable —— XP 可用 */
static int lan_rows(unsigned **ips, unsigned **masks, int *cnt){
	ULONG sz = 0;
	if (GetIpAddrTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER || !sz) sz = 4096;
	PMIB_IPADDRTABLE t = (PMIB_IPADDRTABLE)xmalloc(sz);
	if (GetIpAddrTable(t, &sz, TRUE) != NO_ERROR){ free(t); return 0; }
	int n = 0;
	unsigned *ips2 = (unsigned*)xmalloc(sizeof(unsigned)*32);
	unsigned *ms2 = (unsigned*)xmalloc(sizeof(unsigned)*32);
	for (DWORD i = 0; i < t->dwNumEntries && n < 32; i++){
		unsigned addr = (unsigned)t->table[i].dwAddr;
		unsigned mask = (unsigned)t->table[i].dwMask;
		if (!addr) continue;
		if ((addr & 0xFF) == 127) continue;      /* 回环 */
		ips2[n] = addr; ms2[n] = mask; n++;
	}
	free(t);
	if (!n){ free(ips2); free(ms2); return 0; }
	*ips = ips2; *masks = ms2; *cnt = n;
	return 1;
}
static void addr_str(unsigned a, char *dst){ /* 网络序 → "a.b.c.d" */
	sprintf(dst, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
}
static int ip_is_private(unsigned a){
	unsigned b0 = a & 0xFF, b1 = (a >> 8) & 0xFF;
	return b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168);
}
/* lanIPv4s：私网优先，各组按字典序（与 Go 版一致） */
static void lan_list(Aren *a, char ***out, int *cnt){
	unsigned *ips, *masks; int n = 0;
	*out = NULL; *cnt = 0;
	if (!lan_rows(&ips, &masks, &n)){
		char **r = (char**)am(a, sizeof(char*));
		r[0] = ams(a, "127.0.0.1");
		*out = r; *cnt = 1;
		return;
	}
	char *all[64]; int priv[64], pub[64];
	int np = 0, nu = 0;
	for (int i = 0; i < n && i < 64; i++){
		char b[20];
		addr_str(ips[i], b);
		all[i] = ams(a, b);
		if (ip_is_private(ips[i])) priv[np++] = i;
		else pub[nu++] = i;
	}
	for (int i = 0; i < np; i++) for (int j = i+1; j < np; j++)
		if (strcmp(all[priv[i]], all[priv[j]]) > 0){ int t = priv[i]; priv[i] = priv[j]; priv[j] = t; }
	for (int i = 0; i < nu; i++) for (int j = i+1; j < nu; j++)
		if (strcmp(all[pub[i]], all[pub[j]]) > 0){ int t = pub[i]; pub[i] = pub[j]; pub[j] = t; }
	char **r = (char**)am(a, sizeof(char*)*n);
	int m = 0;
	for (int i = 0; i < np; i++) r[m++] = all[priv[i]];
	for (int i = 0; i < nu; i++) r[m++] = all[pub[i]];
	free(ips); free(masks);
	*out = r; *cnt = m;
}
static void primaryURL(char *dst, size_t cap){
	Aren a; memset(&a, 0, sizeof(a));
	char **ips; int n;
	lan_list(&a, &ips, &n);
	sprintf(dst, "http://%s:%d", ips[0], g_port);
	ar_free(&a);
}
static DWORD WINAPI bcastProc(LPVOID u){
	SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET) return 0;
	BOOL bc = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bc, sizeof(bc));
	for (;;){
		char url[128];
		primaryURL(url, sizeof(url));
		SB p; memset(&p, 0, sizeof(p));
		sb_str(&p, "{\"id\":"); json_str(&p, g_instance);
		sb_str(&p, ",\"name\":"); json_str(&p, g_nameU8);
		sb_str(&p, ",\"url\":"); json_str(&p, url);
		sb_ch(&p, '}');
		/* 发到：各网段广播地址 + 受限广播 + 回环 的 8 个 beacon 端口 */
		unsigned *ips, *masks; int nr = 0;
		int have = lan_rows(&ips, &masks, &nr);
		unsigned bcs[36]; int nb = 0;
		if (have){
			for (int i = 0; i < nr && nb < 32; i++){
				unsigned bc = ips[i] | ~masks[i];
				int dup = 0;
				for (int j = 0; j < nb; j++) if (bcs[j] == bc) dup = 1;
				if (!dup) bcs[nb++] = bc;
			}
			free(ips); free(masks);
		}
		bcs[nb++] = 0xFFFFFFFFu;   /* 受限广播 */
		bcs[nb++] = 0x0100007Fu;   /* 127.0.0.1 */
		for (int i = 0; i < nb; i++){
			for (int k = 0; k < BEACON_PORTS; k++){
				struct sockaddr_in to;
				memset(&to, 0, sizeof(to));
				to.sin_family = AF_INET;
				to.sin_port = htons((unsigned short)(BEACON_BASE + k));
				to.sin_addr.s_addr = bcs[i];
				sendto(s, p.p, (int)p.n, 0, (struct sockaddr*)&to, sizeof(to));
			}
		}
		free(p.p);
		dev_prune();
		Sleep(2000);
	}
	return 0;
}
static DWORD WINAPI beaconRecvProc(LPVOID u){
	SOCKET s = INVALID_SOCKET;
	for (int i = 0; i < BEACON_PORTS; i++){
		SOCKET t = socket(AF_INET, SOCK_DGRAM, 0);
		if (t == INVALID_SOCKET) continue;
		struct sockaddr_in lo;
		memset(&lo, 0, sizeof(lo));
		lo.sin_family = AF_INET;
		lo.sin_addr.s_addr = INADDR_ANY;
		lo.sin_port = htons((unsigned short)(BEACON_BASE + i));
		if (bind(t, (struct sockaddr*)&lo, sizeof(lo)) == 0){ s = t; break; }
		closesocket(t);
	}
	if (s == INVALID_SOCKET){
		fprintf(stderr, "UDP %d~%d 均被占用，“附近设备”功能受限\n", BEACON_BASE, BEACON_BASE + BEACON_PORTS - 1);
		return 0;
	}
	char buf[4096];
	for (;;){
		struct sockaddr_in from;
		int fl = sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fl);
		if (n <= 0){ Sleep(50); continue; }
		buf[n] = 0;
		Aren a; memset(&a, 0, sizeof(a));
		JV *j = jparse(&a, buf);
		if (j && j->type == 5){
			JV *idv = jget(j, "id");
			if (idv && idv->type == 3 && idv->str[0] && strcmp(idv->str, g_instance)){
				char ip[64];
				addr_str((unsigned)from.sin_addr.s_addr, ip);
				dev_upsert(idv->str, jstr_get(j, "name", ""), jstr_get(j, "url", ""), ip);
			}
		}
		ar_free(&a);
	}
	return 0;
}
static void startDiscovery(void){
	InitializeCriticalSection(&g_devLock);
	CreateThread(NULL, 0, bcastProc, NULL, 0, NULL);
	CreateThread(NULL, 0, beaconRecvProc, NULL, 0, NULL);
}

/* ------------------------------------------------------------------
 * 名称清洗（与 Go 版同规则）
 * ------------------------------------------------------------------ */
static void wc_filter(wchar_t *w){
	/* 逐字符替换非法字符为 '_'（不改变缓冲） */
	for (wchar_t *p = w; *p; ){
		if (*p == L'\\' || *p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
			*p == L'"' || *p == L'<' || *p == L'>' || *p == L'|' || *p < 32)
			*p = L'_';
		p++;
	}
}
static char *sanitizeName(Aren *a, const char *nameU8){
	wchar_t *w = u2w(a, nameU8);
	/* 取 base（先统一分隔符） */
	for (wchar_t *p = w; *p; p++) if (*p == L'\\') *p = L'/';
	wchar_t *b = w;
	for (wchar_t *p = w; *p; p++) if (*p == L'/') b = p + 1;
	if (!wcscmp(b, L".") || !wcscmp(b, L"..")) return ams(a, "");
	wc_filter(b);
	size_t l = wcslen(b);                 /* 截断（对应 Go 的 200 字节上限） */
	if (l > 200) b[l = 200] = 0;
	while (l && (b[l-1] == L' ' || b[l-1] == L'.')) b[--l] = 0;
	return w2u(a, b);
}
static char *sanitizeRelDir(Aren *a, const char *dirU8){
	wchar_t *w = u2w(a, dirU8);
	SB out; memset(&out, 0, sizeof(out));
	wchar_t *ctx = NULL, *tok;
	int count = 0;
	for (wchar_t *p = w; *p; p++) if (*p == L'\\') *p = L'/';
	tok = wcstok(w, L"/", &ctx);
	while (tok && count < 20){
		wc_filter(tok);
		if (wcscmp(tok, L".") && wcscmp(tok, L"..") && tok[0]){
			size_t l = wcslen(tok);
			while (l && (tok[l-1] == L' ' || tok[l-1] == L'.')) tok[--l] = 0;
			if (tok[0]){
				if (count) sb_str(&out, "/");
				sb_w(&out, tok);
				count++;
			}
		}
		tok = wcstok(NULL, L"/", &ctx);
	}
	char *r = (char*)am(a, out.n + 1);
	if (out.n) memcpy(r, out.p, out.n);
	r[out.n] = 0;
	free(out.p);
	return r;
}
/* safePath：把 utf 相对路径限制在共享目录内 → 宽绝对路径 */
static int safePathW(Aren *a, const char *relU8, wchar_t *out, char **relEcho){
	/* 拆段清洗 */
	wchar_t *w = u2w(a, relU8 ? relU8 : "");
	wchar_t *segs[64];
	int n = 0;
	wchar_t *ctx = NULL, *tok;
	for (wchar_t *p = w; *p; p++) if (*p == L'\\') *p = L'/';
	tok = wcstok(w, L"/", &ctx);
	while (tok){
		if (!wcscmp(tok, L"..")) return 0;
		if (tok[0] && wcscmp(tok, L".")){
			wc_filter(tok);
			size_t l = wcslen(tok);
			while (l && tok[l-1] == ' ') tok[--l] = 0;
			if (tok[0]){
				if (n >= 60) return 0;
				segs[n++] = tok;
			}
		}
		tok = wcstok(NULL, L"/", &ctx);
	}
	wcscpy(out, g_rootW);
	SB echo; memset(&echo, 0, sizeof(echo));
	for (int i = 0; i < n; i++){
		if (!wccat(out, PWCAP, L"\\")) return 0;
		if (!wccat(out, PWCAP, segs[i])) return 0;
		if (i) sb_str(&echo, "/");
		sb_w(&echo, segs[i]);
	}
	if (relEcho){
		char *r = (char*)am(a, echo.n + 1);
		if (echo.n) memcpy(r, echo.p, echo.n);
		r[echo.n] = 0;
		*relEcho = r;
	}
	free(echo.p);
	return 1;
}

/* ------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------ */
typedef struct {
	const char *method;
	const char *ver;
	const char *range;
	const char *auth;
	size_t cl;
	int closeAfter;   /* 请求端要求或 HTTP/1.0 */
	int expect100;
	char *path;       /* 解码后带导 '/' 的规范化路径 */
	char **qk, **qv; int qn;
	const char *body; size_t bodyN;
	char ip[64];
	SOCKET s;
} Rq;

static const char *qget(Rq *r, const char *k){
	for (int i = 0; i < r->qn; i++) if (!strcmp(r->qk[i], k)) return r->qv[i];
	return NULL;
}

static const char *status_txt(int c){
	switch (c){
		case 200: return "200 OK";
		case 206: return "206 Partial Content";
		case 400: return "400 Bad Request";
		case 401: return "401 Unauthorized";
		case 403: return "403 Forbidden";
		case 404: return "404 Not Found";
		case 405: return "405 Method Not Allowed";
		case 409: return "409 Conflict";
		case 413: return "413 Payload Too Large";
		case 416: return "416 Requested Range Not Satisfiable";
		case 500: return "500 Internal Server Error";
		default:  return "500 Internal Server Error";
	}
}
static int hdr_send(SOCKET s, int code, const char *ctype, long long cl, const char *extras, int keep){
	SB h; memset(&h, 0, sizeof(h));
	sb_str(&h, "HTTP/1.1 ");
	sb_str(&h, status_txt(code));
	sb_str(&h, "\r\nContent-Type: ");
	sb_str(&h, ctype);
	if (cl >= 0){
		sb_str(&h, "\r\nContent-Length: ");
		sb_i64(&h, cl);
	}
	sb_str(&h, "\r\nConnection: ");
	sb_str(&h, keep ? "keep-alive" : "close");
	if (extras){
		sb_str(&h, "\r\n");      /* Connection 行尾 */
		sb_str(&h, extras);      /* extras 自带行尾 \r\n */
		sb_str(&h, "\r\n");      /* 空行：头结束 */
	} else {
		sb_str(&h, "\r\n\r\n");
	}
	int ok = sock_sendall(s, h.p, h.n);
	free(h.p);
	return ok;
}
static void resp_json(Aren *a, Rq *r, int code, SB *j){
	const char *ex = NULL;
	char exb[64];
	sprintf(exb, "Cache-Control: no-cache\r\n");
	ex = exb;
	if (!hdr_send(r->s, code, "application/json; charset=utf-8", (long long)j->n, ex, 1))
		return;
	if (j->n) sock_sendall(r->s, j->p, j->n);
}
static void sb_err(Aren *a, SB *s, const char *msg){
	sb_str(s, "{\"error\":");
	json_str(s, msg);
	sb_ch(s, '}');
}
static void resp_err(Aren *a, Rq *r, int code, const char *msg){
	SB s; memset(&s, 0, sizeof(s));
	sb_err(a, &s, msg);
	resp_json(a, r, code, &s);
	free(s.p);
}
static void resp_ok(Aren *a, Rq *r){
	SB s; memset(&s, 0, sizeof(s));
	sb_str(&s, "{\"ok\":true}");
	resp_json(a, r, 200, &s);
	free(s.p);
}

/* MIME（供下载用） */
static const char *mime_of(const char *name){
	static const struct { const char *ext, *mt; } MT[] = {
		{ "txt",  "text/plain; charset=utf-8" },  { "md", "text/plain; charset=utf-8" },
		{ "log",  "text/plain; charset=utf-8" },  { "csv", "text/plain; charset=utf-8" },
		{ "ini",  "text/plain; charset=utf-8" },  { "yml", "text/plain; charset=utf-8" },
		{ "yaml", "text/plain; charset=utf-8" },  { "json", "text/plain; charset=utf-8" },
		{ "html", "text/html; charset=utf-8" },   { "htm", "text/html; charset=utf-8" },
		{ "js",   "text/javascript; charset=utf-8" }, { "css", "text/css; charset=utf-8" },
		{ "png",  "image/png" },   { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
		{ "gif",  "image/gif" },   { "bmp", "image/bmp" },  { "webp", "image/webp" },
		{ "svg",  "image/svg+xml" }, { "ico", "image/x-icon" },
		{ "mp4",  "video/mp4" },   { "webm", "video/webm" },
		{ "mp3",  "audio/mpeg" },  { "wav", "audio/wav" },  { "ogg", "audio/ogg" }, { "m4a", "audio/x-m4a" },
		{ "pdf",  "application/pdf" }, { "zip", "application/zip" },
	};
	const char *ext = name;
	for (const char *p = name; *p; p++) if (*p == '.') ext = p + 1;
	for (size_t i = 0; i < sizeof(MT)/sizeof(MT[0]); i++)
		if (!_stricmp(ext, MT[i].ext)) return MT[i].mt;
	return "application/octet-stream";
}

/* Range 头解析 → [start,end)（端闭开），不满意返回 -1，无效返回 -2（按 200 处理） */
static void range_parse(const char *h, long long size, long long *st, long long *en){
	*st = 0; *en = size - 1;
	if (*st > *en) *en = -1;
	if (!h || !size) return;
	if (strncmp(h, "bytes=", 6)) return;
	if (strchr(h + 6, ',')) return;        /* 多区间不支持 → 200 全量 */
	const char *q = h + 6;
	char aa[24], bb[24];
	int i = 0;
	while (*q && *q != '-' && i < 23) aa[i++] = *q++;
	aa[i] = 0;
	if (*q != '-') return;
	q++;
	i = 0;
	while (*q && i < 23){ if (*q < '0' || *q > '9') return; bb[i++] = *q++; }
	bb[i] = 0;
	while (*q == ' ') q++;
	if (*q) return;
	int hasA = aa[0] != 0, hasB = bb[0] != 0;
	if (hasA && hasB){
		long long a = j_atoll(aa, NULL), b = j_atoll(bb, NULL);
		if (a > b || a >= size || b >= size){ if (a >= size) *st = -1; return; }
		*st = a; *en = b;
	} else if (hasA){
		long long a = j_atoll(aa, NULL);
		if (a >= size){ *st = -1; return; }
		*st = a;
	} else if (hasB){
		long long b = j_atoll(bb, NULL);
		if (b <= 0) return;
		long long a = size - b; if (a < 0) a = 0;
		*st = a;
	} else return;
}

/* /d/ 单文件下载（Range 206 / HEAD / 事件 send） */
static void hDownload(Aren *a, Rq *r, const char *rel){
	wchar_t abs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, rel, abs, &echo)){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	FINFO fi;
	if (!wstat(abs, &fi)){
		resp_err(a, r, 404, "文件不存在（目录请使用打包下载）");
		return;
	}
	if (fi.isDir){
		resp_err(a, r, 404, "文件不存在（目录请使用打包下载）");
		return;
	}
	HANDLE f = CreateFileW(abs, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE){
		resp_err(a, r, 403, "无法打开文件");
		return;
	}
	/* Content-Disposition */
	SB cd; memset(&cd, 0, sizeof(cd));
	const char *base = rel; /* 最后一段 */
	for (const char *p = rel; *p; p++) if (*p == '/') base = p + 1;
	int ascii = 1;
	for (const unsigned char *p = (const unsigned char*)base; *p; p++)
		if (*p <= 0x20 || *p >= 0x7F){ ascii = 0; break; }
	sb_str(&cd, "Content-Disposition: attachment; ");
	if (ascii){
		sb_str(&cd, "filename=\"");
		for (const char *p = base; *p; p++){
			if (*p == '"'){ sb_add(&cd, "%22", 3); continue; }
			sb_ch(&cd, *p);
		}
		sb_ch(&cd, '"');
	} else {
		sb_str(&cd, "filename*=UTF-8''");
		pctenc(&cd, base);
	}
	sb_str(&cd, "\r\n");
	long long start = 0, end = fi.size - 1, code = 200;
	if (fi.size == 0){ start = 0; end = -1; }
	else {
		range_parse(r->range, fi.size, &start, &end);
		if (start == -1){
			char cr[96];
			sprintf(cr, "Content-Range: bytes */%lld\r\n", fi.size);
			SB ex2; memset(&ex2, 0, sizeof(ex2));
			sb_str(&ex2, cr);
			sb_str(&ex2, cd.p);
			hdr_send(r->s, 416, "text/plain; charset=utf-8", 0, ex2.p, 1);
			sock_sendall(r->s, "invalid range\n", 14);
			free(ex2.p); free(cd.p);
			CloseHandle(f);
			return;
		}
		if (end >= start) code = (start == 0 && end == fi.size - 1) ? 200 : 206;
	}
	char cr[96];
	SB ex2; memset(&ex2, 0, sizeof(ex2));
	if (code == 206){
		sprintf(cr, "Content-Range: bytes %lld-%lld/%lld\r\n", start, end, fi.size);
		sb_str(&ex2, cr);
	}
	sb_str(&ex2, cd.p);
	long long len = (end >= start) ? (end - start + 1) : 0;
	if (!hdr_send(r->s, code, mime_of(base), len, ex2.p, 1)){
		free(ex2.p); free(cd.p); CloseHandle(f);
		return;
	}
	free(ex2.p); free(cd.p);
	if (len > 0 && strcmp(r->method, "HEAD")){
		LARGE_INTEGER sv;
		sv.QuadPart = start;
		SetFilePointerEx(f, sv, NULL, FILE_BEGIN);
		static const DWORD BZ = 1 << 20;
		unsigned char *b = (unsigned char*)xmalloc(BZ);
		long long left = len;
		while (left > 0){
			DWORD want = (DWORD)(left > (long long)BZ ? BZ : left);
			DWORD got = 0;
			if (!ReadFile(f, b, want, &got, NULL) || !got) break;
			if (!sock_sendall(r->s, (const char*)b, got)) break;
			left -= got;
		}
		free(b);
	}
	CloseHandle(f);
	if (!strcmp(r->method, "GET"))
		store_addEvent("send", ams(a, base), fi.size, r->ip);
}

/* /zip/ 流式打包（STORE + UTF-8 名） */
static void hZip(Aren *a, Rq *r, const char *rel){
	wchar_t abs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, rel, abs, &echo)){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	FINFO fi;
	if (!wstat(abs, &fi) || !fi.isDir){
		resp_err(a, r, 404, "目录不存在");
		return;
	}
	const char *base = rel; /* 目录名（最后一段） */
	for (const char *p = rel; *p; p++) if (*p == '/') base = p + 1;
	SB zname; memset(&zname, 0, sizeof(zname));
	sb_str(&zname, base);
	sb_str(&zname, ".zip");
	SB cd; memset(&cd, 0, sizeof(cd));
	sb_str(&cd, "Content-Disposition: attachment; ");
	SB zp; memset(&zp, 0, sizeof(zp));
	pctenc(&zp, zname.p);
	int zascii = 1;
	for (const unsigned char *p = (const unsigned char*)zname.p; *p; p++)
		if (*p <= 0x20 || *p >= 0x7F){ zascii = 0; break; }
	if (zascii){
		sb_str(&cd, "filename=\"");
		for (const char *p = zname.p; *p; p++) sb_ch(&cd, *p);
		sb_ch(&cd, '"');
	} else sb_str(&cd, "filename*=UTF-8''");
	/* 修正：ascii 分支已写完；非 ascii 补 percent */
	if (!zascii) sb_add(&cd, zp.p, zp.n);
	sb_str(&cd, "\r\n");
	free(zp.p);

	ZWItem *items = NULL; int n = 0, cap = 0;
	zipWalk(abs, "", a, &items, &n, &cap, 0);

	r->closeAfter = 1; /* 流式：无 Content-Length，把本请求按 Connection: close 结束 */

	SOCKET s = r->s;
	if (!hdr_send(s, 200, "application/zip", -1, cd.p, 0)){
		free(cd.p); free(zname.p); free(items);
		return;
	}
	free(cd.p);

	if (!crc32_ready) crc32_init();
	unsigned char hbuf[128];
	long long pos = 0;
	int vn = 0;
	unsigned *vcrc  = (unsigned*)xmalloc(sizeof(unsigned)*(size_t)(n ? n : 1));
	unsigned *vdate = (unsigned*)xmalloc(sizeof(unsigned)*(size_t)(n ? n : 1));
	unsigned *vtime = (unsigned*)xmalloc(sizeof(unsigned)*(size_t)(n ? n : 1));
	unsigned long long *voff = (unsigned long long*)xmalloc(sizeof(unsigned long long)*(size_t)(n ? n : 1));
	int *vlist = (int*)xmalloc(sizeof(int)*(size_t)(n ? n : 1));
	unsigned char *big = (unsigned char*)xmalloc(1 << 20);

	/* 预扫描：CRC + DOS 时间（本地头必须带完整 CRC；无法访问的条目跳过） */
	for (int i = 0; i < n; i++){
		if (strlen(items[i].rel) > 65535) continue;
		HANDLE f = CreateFileW(items[i].abs, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE) continue;
		unsigned c = 0xFFFFFFFFu;
		for (;;){
			DWORD got = 0;
			if (!ReadFile(f, big, 1 << 20, &got, NULL) || !got) break;
			c = crc32_add(c, big, got);
		}
		CloseHandle(f);
		unsigned dd = 0, dt = 0;
		dosTime(&items[i].ft, &dd, &dt);
		vcrc[vn] = ~c; vdate[vn] = dd; vtime[vn] = dt; vlist[vn] = i; vn++;
	}

	/* 双遍写出：本地头 + 数据流 */
	int dead = 0;
	for (int vi = 0; vi < vn && !dead; vi++){
		int i = vlist[vi];
		HANDLE f = CreateFileW(items[i].abs, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE) continue;
		size_t nl = strlen(items[i].rel);
		zi_w32(hbuf, 0x04034b50u);
		zi_w16(hbuf + 4, 20);
		zi_w16(hbuf + 6, 0x0800u);          /* UTF-8 名 */
		zi_w16(hbuf + 8, 0);                /* STORE */
		zi_w16(hbuf + 10, vtime[vi]);
		zi_w16(hbuf + 12, vdate[vi]);
		zi_w32(hbuf + 14, vcrc[vi]);
		zi_w32(hbuf + 18, (unsigned long)items[i].size);
		zi_w32(hbuf + 22, (unsigned long)items[i].size);
		zi_w16(hbuf + 26, (unsigned)nl);
		zi_w16(hbuf + 28, 0);
		voff[vi] = (unsigned long long)pos;
		if (!sock_sendall(s, (const char*)hbuf, 30)){ dead = 1; CloseHandle(f); break; }
		pos += 30;
		if (!sock_sendall(s, items[i].rel, nl)){ dead = 1; CloseHandle(f); break; }
		pos += (long long)nl;
		for (;;){
			DWORD got = 0;
			if (!ReadFile(f, big, 1 << 20, &got, NULL)){ dead = 1; break; }
			if (!got) break;
			if (!sock_sendall(s, (const char*)big, got)){ dead = 1; break; }
			pos += got;
		}
		CloseHandle(f);
	}
	free(big);

	/* 中央目录 + EOCD */
	if (!dead){
		SB ctr; memset(&ctr, 0, sizeof(ctr));
		for (int vi = 0; vi < vn; vi++){
			int i = vlist[vi];
			size_t nl = strlen(items[i].rel);
			unsigned char h[46];
			zi_w32(h, 0x02014b50u);
			zi_w16(h + 4, 20); zi_w16(h + 6, 20);
			zi_w16(h + 8, 0x0800u); zi_w16(h + 10, 0);
			zi_w16(h + 12, vtime[vi]); zi_w16(h + 14, vdate[vi]);
			zi_w32(h + 16, vcrc[vi]);
			zi_w32(h + 20, (unsigned long)items[i].size);
			zi_w32(h + 24, (unsigned long)items[i].size);
			zi_w16(h + 28, (unsigned)nl);
			zi_w16(h + 30, 0); zi_w16(h + 32, 0);
			zi_w16(h + 34, 0); zi_w16(h + 36, 0);
			zi_w32(h + 38, 0);
			zi_w32(h + 42, (unsigned long)voff[vi]);
			sb_add(&ctr, h, 46);
			sb_add(&ctr, items[i].rel, nl);
		}
		unsigned char eocd[22];
		zi_w32(eocd, 0x06054b50u);
		zi_w16(eocd + 4, 0); zi_w16(eocd + 6, 0);
		zi_w16(eocd + 8, (unsigned)vn); zi_w16(eocd + 10, (unsigned)vn);
		zi_w32(eocd + 12, (unsigned long)ctr.n);
		zi_w32(eocd + 16, (unsigned long)pos);
		zi_w16(eocd + 20, 0);
		sock_sendall(s, ctr.p, ctr.n);
		sock_sendall(s, (const char*)eocd, 22);
		store_addEvent("send", zname.p, pos, r->ip);
		free(ctr.p);
	}
	free(vcrc); free(vdate); free(vtime); free(voff); free(vlist);
	free(items);
	free(zname.p);
	Sleep(1); /* 保证对端读完再关（Connection: close 流式） */
}

/* ------------------- 其他 API handlers ------------------- */
static void hInfo(Aren *a, Rq *r){
	SB s; memset(&s, 0, sizeof(s));
	sb_str(&s, "{\"name\":"); json_str(&s, g_nameU8);
	sb_str(&s, ",\"root\":"); json_str(&s, g_rootU8);
	char *ddu = w2u(a, g_dataW);
	sb_str(&s, ",\"dataDir\":"); json_str(&s, ddu);
	sb_str(&s, ",\"port\":"); sb_i64(&s, g_port);
	char **ips; int n;
	lan_list(a, &ips, &n);
	sb_str(&s, ",\"urls\":[");
	for (int i = 0; i < n; i++){
		if (i) sb_ch(&s, ',');
		char b[128];
		sprintf(b, "http://%s:%d", ips[i], g_port);
		json_str(&s, b);
	}
	sb_str(&s, "]");
	sb_str(&s, ",\"hasPassword\":");
	sb_str(&s, g_pass[0] ? "true" : "false");
	sb_ch(&s, '}');
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void hList(Aren *a, Rq *r){
	const char *qp = qget(r, "path");
	wchar_t abs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, qp ? qp : "", abs, &echo)){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	wchar_t pat[PWCAP];
	wcscpy(pat, abs);
	if (!wccat(pat, PWCAP, L"\\*")){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE){
		resp_err(a, r, 404, "无法读取目录");
		return;
	}
	SB s; memset(&s, 0, sizeof(s));
	sb_str(&s, "{\"path\":");
	json_str(&s, echo ? echo : (qp ? qp : ""));
	sb_str(&s, ",\"entries\":[");
	int first = 1;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		if (fd.cFileName[0] == L'.') continue;
		char *nm = w2u(a, fd.cFileName);
		int isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		long long sz = isDir ? 0 : (long long)(((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
		ULONGLONG ft = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
		long long mt = ft < 116444736000000000ULL ? 0 : (long long)((ft - 116444736000000000ULL) / 10000000ULL);
		if (!first) sb_ch(&s, ',');
		first = 0;
		sb_str(&s, "{\"name\":"); json_str(&s, nm);
		sb_str(&s, ",\"size\":"); sb_i64(&s, sz);
		sb_str(&s, ",\"mtime\":"); sb_i64(&s, mt);
		sb_str(&s, ",\"isDir\":");
		sb_str(&s, isDir ? "true" : "false");
		sb_ch(&s, '}');
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	sb_str(&s, "]}");
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void hUploadInit(Aren *a, Rq *r){
	JV *j = (r->bodyN && r->body) ? jparse(a, r->body) : NULL;
	if (!j || j->type != 5){
		resp_err(a, r, 400, "请求格式错误");
		return;
	}
	const char *nm0 = jstr_get(j, "name", "");
	long long size = jnum_get(j, "size", 0);
	const char *dir0 = jstr_get(j, "dir", "");
	char *name = sanitizeName(a, nm0);
	if (!name[0]){
		resp_err(a, r, 400, "无效的文件名");
		return;
	}
	char *relDir = sanitizeRelDir(a, dir0);
	wchar_t dirabs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, relDir, dirabs, &echo)){
		resp_err(a, r, 400, "无效的目标目录");
		return;
	}
	/* 建目录（若分段） */
	wchar_t *dir0w = u2w(a, dir0);
	wchar_t *segs[24]; int ns = 0;
	wchar_t *ctx = NULL, *tok;
	wchar_t tmpD[PWCAP];
	wcscpy(tmpD, dir0w);
	for (wchar_t *p = tmpD; *p; p++) if (*p == L'\\') *p = L'/';
	tok = wcstok(tmpD, L"/", &ctx);
	while (tok && ns < 20){
		if (tok[0] && wcscmp(tok, L".") && wcscmp(tok, L"..")) segs[ns++] = tok;
		tok = wcstok(NULL, L"/", &ctx);
	}
	if (ns && !mkdirSubW(g_rootW, segs, ns)){
		resp_err(a, r, 500, "无法创建目标目录");
		return;
	}
	char idbuf[40];
	randHex(idbuf, 8);
	wchar_t *nw = u2w(a, name);
	wchar_t tmpName[560], shortName[520];
	_snwprintf(shortName, 519, L"%hs", idbuf);
	size_t nl = wcslen(nw);
	size_t room = 500 - wcslen(shortName);
	if (nl > room) nl = room;
	memcpy(tmpName, nw, nl * sizeof(wchar_t));
	tmpName[nl] = 0;
	wchar_t tmpPath[PWCAP];
	wcscpy(tmpPath, dirabs);
	if (!wccat(tmpPath, PWCAP, L"\\")){ resp_err(a, r, 500, "无法创建临时文件"); return; }
	if (!wccat(tmpPath, PWCAP, TEMP_PREFIX_W)){ resp_err(a, r, 500, "无法创建临时文件"); return; }
	if (!wccat(tmpPath, PWCAP, shortName)){ resp_err(a, r, 500, "无法创建临时文件"); return; }
	if (!wccat(tmpPath, PWCAP, L"-")){ resp_err(a, r, 500, "无法创建临时文件"); return; }
	if (!wccat(tmpPath, PWCAP, tmpName)){ resp_err(a, r, 500, "无法创建临时文件"); return; }
	HANDLE f = CreateFileW(tmpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE){
		resp_err(a, r, 500, "无法创建临时文件");
		return;
	}
	CloseHandle(f);
	Sess *s = (Sess*)xmalloc(sizeof(Sess));
	memset(s, 0, sizeof(Sess));
	InitializeCriticalSection(&s->cs);
	strcpy(s->id, idbuf);
	wcscpy(s->tmpW, tmpPath);
	s->dirRel = _strdup(relDir);
	s->display = _strdup(name);
	s->size = size;
	s->last = time(NULL);
	sess_add(s);
	SB out; memset(&out, 0, sizeof(out));
	sb_str(&out, "{\"id\":");
	json_str(&out, s->id);
	sb_ch(&out, '}');
	resp_json(a, r, 200, &out);
	free(out.p);
}
static void hUploadChunk(Aren *a, Rq *r, const char *id){
	int code404 = 0;
	Sess *s = sess_find(a, id, &code404);
	if (!s){
		resp_err(a, r, 404, code404 == 1 ? "无效的会话 ID" : "上传会话不存在（可能服务已重启），请重新选择文件上传");
		return;
	}
	const char *os = qget(r, "offset");
	if (!os || !os[0]){
		resp_err(a, r, 400, "无效的偏移量");
		return;
	}
	int bad = 0;
	for (const char *p = os; *p; p++) if (*p < '0' || *p > '9') bad = 1;
	long long off = bad ? -1 : j_atoll(os, NULL);
	if (off < 0 && off != 0) bad = 1; /* 全数字以外一律拒绝 */
	if (bad){
		resp_err(a, r, 400, "无效的偏移量");
		return;
	}
	EnterCriticalSection(&s->cs);
	s->last = time(NULL);
	if (s->done){
		long long rc = s->received;
		LeaveCriticalSection(&s->cs);
		SB o; memset(&o, 0, sizeof(o));
		sb_str(&o, "{\"error\":\"会话已结束\",\"received\":");
		sb_i64(&o, rc);
		sb_ch(&o, '}');
		resp_json(a, r, 409, &o);
		free(o.p);
		return;
	}
	if (off != s->received){
		long long rc = s->received;
		LeaveCriticalSection(&s->cs);
		SB o; memset(&o, 0, sizeof(o));
		sb_str(&o, "{\"received\":");
		sb_i64(&o, rc);
		sb_str(&o, ",\"mismatch\":true}");
		resp_json(a, r, 200, &o);
		free(o.p);
		return;
	}
	/* 追加 body（收到多少写多少：body 可能没读全？读请求时已保证 CL 全量） */
	int wr = 1;
	if (r->bodyN){
		HANDLE f = CreateFileW(s->tmpW, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE) wr = 0;
		else {
			DWORD done = 0;
			size_t total = (size_t)r->bodyN;
			while (done < total){
				DWORD w = 0;
				if (!WriteFile(f, r->body + done, (DWORD)((total - done) > 0x40000000 ? 0x40000000 : (total - done)), &w, NULL) || !w){ wr = 0; break; }
				done += w;
			}
			CloseHandle(f);
		}
	}
	long long rc;
	if (wr){ s->received += (long long)r->bodyN; rc = s->received; LeaveCriticalSection(&s->cs); }
	else {
		rc = s->received;
		LeaveCriticalSection(&s->cs);
		SB o; memset(&o, 0, sizeof(o));
		sb_str(&o, "{\"error\":\"写入失败\",\"received\":");
		sb_i64(&o, rc);
		sb_ch(&o, '}');
		resp_json(a, r, 500, &o);
		free(o.p);
		return;
	}
	SB o; memset(&o, 0, sizeof(o));
	sb_str(&o, "{\"received\":");
	sb_i64(&o, rc);
	sb_ch(&o, '}');
	resp_json(a, r, 200, &o);
	free(o.p);
}
static void hUploadStatus(Aren *a, Rq *r, const char *id){
	int code404 = 0;
	Sess *s = sess_find(a, id, &code404);
	if (!s){
		resp_err(a, r, 404, code404 == 1 ? "无效的会话 ID" : "上传会话不存在（可能服务已重启），请重新选择文件上传");
		return;
	}
	EnterCriticalSection(&s->cs);
	long long rc = s->received, sz = s->size;
	LeaveCriticalSection(&s->cs);
	SB o; memset(&o, 0, sizeof(o));
	sb_str(&o, "{\"received\":");
	sb_i64(&o, rc);
	sb_str(&o, ",\"size\":");
	sb_i64(&o, sz);
	sb_ch(&o, '}');
	resp_json(a, r, 200, &o);
	free(o.p);
}
static void hUploadComplete(Aren *a, Rq *r, const char *id){
	int code404 = 0;
	Sess *s = sess_find(a, id, &code404);
	if (!s){
		resp_err(a, r, 404, code404 == 1 ? "无效的会话 ID" : "上传会话不存在（可能服务已重启），请重新选择文件上传");
		return;
	}
	EnterCriticalSection(&s->cs);
	if (s->done){
		long long rc = s->received;
		LeaveCriticalSection(&s->cs);
		/* “会话已结束（已接收 N 字节）” */
		SB m; memset(&m, 0, sizeof(m));
		sb_str(&m, "会话已结束（已接收 ");
		sb_i64(&m, rc);
		sb_str(&m, " 字节）");
		resp_err(a, r, 409, m.p);
		free(m.p);
		return;
	}
	s->done = 1;
	long long rc = s->received;
	wchar_t tmpKeep[PWCAP];
	wcscpy(tmpKeep, s->tmpW);
	char relKeep[1024], dispKeep[560];
	strncpy(relKeep, s->dirRel, sizeof(relKeep) - 1);
	relKeep[sizeof(relKeep) - 1] = 0;
	strncpy(dispKeep, s->display, sizeof(dispKeep) - 1);
	dispKeep[sizeof(dispKeep) - 1] = 0;
	LeaveCriticalSection(&s->cs);

	wchar_t dirabs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, relKeep, dirabs, &echo)){
		resp_err(a, r, 500, "目标目录不可用");
		return;
	}
	wchar_t finalW[PWCAP];
	uniqueNameW(a, dirabs, dispKeep, finalW);
	int moved = MoveFileW(tmpKeep, finalW);
	if (!moved){
		if (!copyFileW(tmpKeep, finalW)){
			/* 失败：清理会话与临时文件 */
			EnterCriticalSection(&g_sesLock);
			sess_remove_locked(s);
			LeaveCriticalSection(&g_sesLock);
			resp_err(a, r, 500, "保存文件失败");
			return;
		}
		DeleteFileW(tmpKeep);
	}
	char *fin = w2u(a, finalW);
	const char *fb = fin;
	for (const char *p = fin; *p; p++) if (*p == '\\') fb = p + 1;
	store_addEvent("receive", fb, rc, r->ip);
	EnterCriticalSection(&g_sesLock);
	sess_remove_locked(s);
	LeaveCriticalSection(&g_sesLock);
	SB o; memset(&o, 0, sizeof(o));
	sb_str(&o, "{\"name\":");
	json_str(&o, fb);
	sb_str(&o, ",\"size\":");
	sb_i64(&o, rc);
	sb_ch(&o, '}');
	resp_json(a, r, 200, &o);
	free(o.p);
}
static void hUploadAbort(Aren *a, Rq *r, const char *id){
	if (hex16ok(id)){
		Sess *s = sess_get(id);
		if (s){
			EnterCriticalSection(&g_sesLock);
			sess_remove_locked(s);
			LeaveCriticalSection(&g_sesLock);
		}
	}
	resp_ok(a, r);
}
static void hDevices(Aren *a, Rq *r){
	dev_prune();
	typedef struct { const char *id, *name, *url, *ip; } DRow;
	DRow rows[128];
	int n = 0;
	EnterCriticalSection(&g_devLock);
	for (Dev *d = g_devs; d && n < 128; d = d->next){
		rows[n].id = ams(a, d->id);
		rows[n].name = ams(a, d->nameU8);
		rows[n].url = ams(a, d->url);
		rows[n].ip = ams(a, d->ip);
		n++;
	}
	LeaveCriticalSection(&g_devLock);
	for (int i = 0; i < n; i++) for (int j = i+1; j < n; j++)
		if (strcmp(rows[i].name, rows[j].name) > 0){ DRow t = rows[i]; rows[i] = rows[j]; rows[j] = t; }
	SB s; memset(&s, 0, sizeof(s));
	sb_str(&s, "{\"devices\":[");
	for (int i = 0; i < n; i++){
		if (i) sb_ch(&s, ',');
		sb_str(&s, "{\"id\":"); json_str(&s, rows[i].id);
		sb_str(&s, ",\"name\":"); json_str(&s, rows[i].name);
		sb_str(&s, ",\"url\":"); json_str(&s, rows[i].url);
		sb_str(&s, ",\"ip\":"); json_str(&s, rows[i].ip);
		sb_ch(&s, '}');
	}
	sb_str(&s, "]}");
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void hTextsGet(Aren *a, Rq *r){
	SB s; memset(&s, 0, sizeof(s));
	EnterCriticalSection(&g_stLock);
	sb_str(&s, "{\"texts\":[");
	for (int i = g_tn - 1; i >= 0; i--){
		if (i != g_tn - 1) sb_ch(&s, ',');
		sb_str(&s, "{\"id\":"); sb_i64(&s, g_texts[i].id);
		sb_str(&s, ",\"content\":"); json_str(&s, g_texts[i].content);
		sb_str(&s, ",\"time\":"); sb_i64(&s, g_texts[i].time);
		sb_ch(&s, '}');
	}
	sb_ch(&s, ']');
	sb_ch(&s, '}');
	LeaveCriticalSection(&g_stLock);
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void preview60(Aren *a, const char *src, char **out){
	/* 前 60 个字符 + “…”（Go 版 rune 语义） */
	size_t i = 0, runes = 0;
	while (src[i] && runes < 60){
		i++;
		while ((src[i] & 0xC0) == 0x80) i++;
		runes++;
	}
	SB s; memset(&s, 0, sizeof(s));
	sb_add(&s, src, i);
	if (src[i]) sb_str(&s, "…");
	char *r = (char*)am(a, s.n + 1);
	if (s.n) memcpy(r, s.p, s.n);
	r[s.n] = 0;
	free(s.p);
	*out = r;
}
static void hTextsPost(Aren *a, Rq *r){
	JV *j = (r->bodyN && r->body) ? jparse(a, r->body) : NULL;
	if (!j || j->type != 5){
		resp_err(a, r, 400, "请求格式错误");
		return;
	}
	const char *c0 = jstr_get(j, "content", "");
	size_t cn = strlen(c0);
	const char *ct = c0; size_t cl = cn;
	utf_trim(&ct, &cl);
	if (!cl){
		resp_err(a, r, 400, "内容不能为空");
		return;
	}
	if (cl > MAXTEXTSIZE){
		resp_err(a, r, 400, "内容过长（上限 200KB）");
		return;
	}
	char *cc = (char*)am(a, cl + 1);
	memcpy(cc, ct, cl); cc[cl] = 0;
	Tx *t = store_addText(cc);
	char *pv;
	preview60(a, t->content, &pv);
	char *ip2 = ams(a, r->ip);
	store_addEvent("text", pv, (long long)cl, ip2);
	SB s; memset(&s, 0, sizeof(s));
	sb_str(&s, "{\"id\":"); sb_i64(&s, t->id);
	sb_str(&s, ",\"content\":"); json_str(&s, t->content);
	sb_str(&s, ",\"time\":"); sb_i64(&s, t->time);
	sb_ch(&s, '}');
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void hTextDelete(Aren *a, Rq *r, const char *idstr){
	long long id = 0;
	const char *p = idstr;
	int neg = 0;
	if (*p == '-'){ neg = 1; p++; }
	if (!*p){
		resp_err(a, r, 400, "无效 ID");
		return;
	}
	for (; *p; p++){
		if (*p < '0' || *p > '9'){
			resp_err(a, r, 400, "无效 ID");
			return;
		}
		id = id * 10 + (*p - '0');
	}
	if (neg) id = -id;
	store_delText(id);
	resp_ok(a, r);
}
static void hHistory(Aren *a, Rq *r){
	SB s; memset(&s, 0, sizeof(s));
	EnterCriticalSection(&g_stLock);
	sb_str(&s, "{\"events\":");
	store_marshallEventsRev(&s);
	sb_ch(&s, '}');
	LeaveCriticalSection(&g_stLock);
	resp_json(a, r, 200, &s);
	free(s.p);
}
static void hHistoryClear(Aren *a, Rq *r){
	store_clearEvents();
	resp_ok(a, r);
}
static void hDelete(Aren *a, Rq *r){
	JV *j = (r->bodyN && r->body) ? jparse(a, r->body) : NULL;
	if (!j || j->type != 5){
		resp_err(a, r, 400, "请求格式错误");
		return;
	}
	const char *p = jstr_get(j, "path", "");
	wchar_t abs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, p, abs, &echo) || !p[0]){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	FINFO fi;
	if (!wstat(abs, &fi)){
		resp_err(a, r, 404, "目标不存在");
		return;
	}
	if (fi.isDir) rmrfW(abs, 0);
	else DeleteFileW(abs);
	const char *nb = p;
	for (const char *q = p; *q; q++) if (*q == '/') nb = q + 1;
	SB ev; memset(&ev, 0, sizeof(ev));
	sb_str(&ev, nb);
	if (fi.isDir) sb_ch(&ev, '/');
	store_addEvent("delete", ev.p, fi.size, r->ip);
	free(ev.p);
	resp_ok(a, r);
}
static void hMkdir(Aren *a, Rq *r){
	JV *j = (r->bodyN && r->body) ? jparse(a, r->body) : NULL;
	if (!j || j->type != 5){
		resp_err(a, r, 400, "请求格式错误");
		return;
	}
	const char *p0 = jstr_get(j, "path", "");
	char *name = sanitizeName(a, p0);
	if (!name[0]){
		resp_err(a, r, 400, "无效的目录名");
		return;
	}
	wchar_t abs[PWCAP]; char *echo = NULL;
	if (!safePathW(a, name, abs, &echo)){
		resp_err(a, r, 400, "非法路径");
		return;
	}
	if (!CreateDirectoryW(abs, NULL)){
		DWORD e = GetLastError();
		if (e == ERROR_ALREADY_EXISTS){
			resp_err(a, r, 409, "同名目录已存在");
			return;
		}
		resp_err(a, r, 500, "创建失败");
		return;
	}
	resp_ok(a, r);
}
static void hQR(Aren *a, Rq *r){
	const char *u0 = qget(r, "u");
	char url[640];
	url[0] = 0;
	if (u0 && u0[0] && (strncmp(u0, "http://", 7) == 0 || strncmp(u0, "https://", 8) == 0) && strlen(u0) <= 500){
		strncpy(url, u0, sizeof(url) - 1);
		url[sizeof(url) - 1] = 0;
	} else primaryURL(url, sizeof(url));
	SB png; memset(&png, 0, sizeof(png));
	if (!qr_png(&png, url)){
		SB e; memset(&e, 0, sizeof(e));
		sb_err(a, &e, "生成二维码失败");
		resp_json(a, r, 500, &e);
		free(e.p);
		return;
	}
	if (hdr_send(r->s, 200, "image/png", (long long)png.n, "Cache-Control: no-cache\r\n", 1))
		sock_sendall(r->s, png.p, png.n);
	free(png.p);
}

/* ------------------------------------------------------------------
 * 请求解析
 * ------------------------------------------------------------------ */
static void rq_pushq(Aren *a, Rq *r, char *k, char *v){
	if (r->qn >= 32) return; /* 项目里 query 参数极少，封顶防溢出 */
	if (r->qn == 0){
		r->qk = (char**)am(a, sizeof(char*)*32);
		r->qv = (char**)am(a, sizeof(char*)*32);
	}
	r->qk[r->qn] = k;
	r->qv[r->qn] = v;
	r->qn++;
}
/* 解析目标 → r->path（带导 '/'）+ query；decode 每一段 */
static void rq_parse_target(Aren *a, Rq *r, const char *raw){
	const char *qm = strchr(raw, '?');
	size_t pl = qm ? (size_t)(qm - raw) : strlen(raw);
	char *rp = (char*)am(a, pl + 1);
	memcpy(rp, raw, pl);
	rp[pl] = 0;
	/* 段解码 */
	SB p; memset(&p, 0, sizeof(p));
	sb_ch(&p, '/');
	const char *seg = rp;
	while (*seg){
		const char *nx = strchr(seg, '/');
		size_t sl = nx ? (size_t)(nx - seg) : strlen(seg);
		if (sl){
			char *d = url_dec(a, seg, sl, 0);
			sb_str(&p, d);
			sb_ch(&p, '/');
		}
		if (!nx) break;
		seg = nx + 1;
	}
	if (p.n > 1) p.n--; /* 尾随 '/' */
	else p.n = 1;   /* 只留常导 */
	p.p[p.n] = 0;
	r->path = p.p;
	if (qm){
		const char *qs = qm + 1;
		while (*qs){
			const char *amp = strchr(qs, '&');
			size_t kl = amp ? (size_t)(amp - qs) : strlen(qs);
			const char *eq = (const char*)memchr(qs, '=', kl);
			size_t kLen = eq ? (size_t)(eq - qs) : kl;
			char *k = url_dec(a, qs, kLen, 1);
			char *v = url_dec(a, eq ? eq + 1 : qs + kLen, eq ? (size_t)(kl - kLen - 1) : (size_t)0, 1);
			rq_pushq(a, r, k, v);
			if (!amp) break;
			qs = amp + 1;
		}
	}
}
typedef struct {
	SOCKET s;
	char ip[64];
	char *rd;
	size_t rcap, rlen, rpos;
} Conn;

typedef struct {
	int bad;         /* 0 ok / 1 解析或 IO 失败 / 2 体积过大 */
	Rq rq;
} ParsedReq;

static int conn_fill(Conn *c, size_t need, int headerOnly){
	/* need==0：至少成功 recv 一次（供“找头”循环用）；need>0：缓冲凑满 need 为止 */
	int got = 0;
	for (;;){
		size_t have = c->rlen - c->rpos;
		if (have >= need && (need || got)) return 1;
		if (c->rcap - c->rlen < 4096){
			size_t nc = c->rcap ? c->rcap*2 : 65536;
			while (nc - c->rlen < 4096) nc *= 2;
			if (nc > (size_t)MAX_BODY*2 + 262144) return 0;
			char *nb = (char*)realloc(c->rd, nc);
			if (!nb) return 0;
			c->rd = nb; c->rcap = nc;
		}
		int w = recv(c->s, c->rd + c->rlen, (int)(c->rcap - c->rlen), 0);
		if (w <= 0){ return (need == 0 && got) ? 1 : 0; }
		c->rlen += (size_t)w;
		got = 1;
		if (need == 0) return 1; /* 一次一发 */
		if (headerOnly && c->rlen - c->rpos > 131072) return 0; /* 头部过大 */
	}
}

static ParsedReq conn_read(Conn *c, Aren *a){
	ParsedReq pr;
	memset(&pr, 0, sizeof(pr));
	/* 找头结束 */
	size_t hdrEnd = 0;
	for (;;){
		if (c->rlen - c->rpos >= 4){
			char *s0 = c->rd + c->rpos;
			size_t win = c->rlen - c->rpos;
			char *f = (char*)memchr(s0, '\r', win - 3);
			while (f){
				if (f[1] == '\n' && f[2] == '\r' && f[3] == '\n'){
					hdrEnd = (size_t)(f - s0) + 4;
					break;
				}
				f = (char*)memchr(f + 1, '\r', (size_t)((c->rd + c->rlen) - (f + 1)));
			}
			if (hdrEnd) break;
		}
		size_t before = c->rlen;
		if (!conn_fill(c, 0, 1)) { pr.bad = 1; return pr; }
		if (c->rlen == before){ pr.bad = 1; return pr; } /* 无进展 */
	}
	Rq *r = &pr.rq;
	r->s = c->s;
	strcpy(r->ip, c->ip);
	char *head = (char*)am(a, hdrEnd + 1);
	memcpy(head, c->rd + c->rpos, hdrEnd);
	head[hdrEnd] = 0;
	c->rpos += hdrEnd;
	/* 逐行 */
	char *line = head;
	char *nline = strstr(line, "\r\n");
	/* 请求行 */
	if (!nline){ pr.bad = 1; return pr; }
	*nline = 0;
	{
		char *sp1 = strchr(line, ' ');
		char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
		if (!sp1 || !sp2){ pr.bad = 1; return pr; }
		*sp1 = 0; *sp2 = 0;
		r->method = ams(a, line);
		rq_parse_target(a, r, sp1 + 1);
		r->ver = ams(a, sp2 + 1);
		if (strncmp(r->ver, "HTTP/1.1", 8)) r->closeAfter = 1;
	}
	line = nline + 2;
	while ((nline = strstr(line, "\r\n")) != NULL){
		*nline = 0;
		if (!line[0]) break; /* 空行（不该出现在此，hdrEnd 已含） */
		char *col = strchr(line, ':');
		if (col){
			*col = 0;
			char *k = line;
			char *v = col + 1;
			while (*v == ' ' || *v == '\t') v++;
			if (!_stricmp(k, "content-length")){
				long long cv = j_atoll(v, NULL);
				if (cv < 0 || cv > (long long)MAX_BODY) cv = (long long)MAX_BODY + 1; /* 触发 413 */
				r->cl = (size_t)cv;
			}
			else if (!_stricmp(k, "expect")){ if (!_stricmp(v, "100-continue")) r->expect100 = 1; }
			else if (!_stricmp(k, "authorization")) r->auth = ams(a, v);
			else if (!_stricmp(k, "range")) r->range = ams(a, v);
			else if (!_stricmp(k, "connection")){ if (_stricmp(v, "keep-alive")) if (!_stricmp(v, "close") || strncmp(r->ver, "HTTP/1.1", 8)) r->closeAfter = 1; }
		}
		line = nline + 2;
	}
	/* 读取 body */
	if (r->cl > 0){
		if (r->cl > (size_t)MAX_BODY){ pr.bad = 2; return pr; } /* 413 */
		if (r->expect100)
			sock_sendall(c->s, "HTTP/1.1 100 Continue\r\n\r\n", 24);
		if (!conn_fill(c, r->cl, 0)){ pr.bad = 1; return pr; }
		r->bodyN = r->cl;
		r->body = (const char*)am(a, r->cl + 1);
		memcpy((void*)r->body, c->rd + c->rpos, r->cl);
		((char*)r->body)[r->cl] = 0;
		c->rpos += r->cl;
	}
	/* 剩余缓冲已消化：压缩 */
	if (c->rpos == c->rlen) c->rpos = c->rlen = 0;
	else if (c->rpos > 0){
		memmove(c->rd, c->rd + c->rpos, c->rlen - c->rpos);
		c->rlen -= c->rpos;
		c->rpos = 0;
	}
	return pr;
}

static void conn_free(Conn *c){
	free(c->rd);
	c->rd = NULL;
}

/* 认证 */
static int auth_ok(Rq *r){
	if (!g_pass[0]) return 1;
	if (!r->auth || strncmp(r->auth, "Basic ", 6)) return 0;
	char raw[384];
	int n = b64_dec(r->auth + 6, raw, sizeof(raw) - 1);
	if (n <= 0) return 0;
	raw[n] = 0;
	char *col = strchr(raw, ':');
	if (!col) return 0;
	*col = 0;
	if (!consteq(raw, g_user)) return 0;
	return consteq(col + 1, g_pass);
}

/* ------------------------------------------------------------------
 * 路由
 * ------------------------------------------------------------------ */
static int route(Aren *a, Rq *r){
	const char *p = r->path;
	const int GET = !strcmp(r->method, "GET");
	const int POST = !strcmp(r->method, "POST");
	const int HEAD = !strcmp(r->method, "HEAD");
	const int DEL = !strcmp(r->method, "DELETE");
	int isGet = GET || HEAD;
	if (!strcmp(p, "/")){
		if (!isGet){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		if (hdr_send(r->s, 200, "text/html; charset=utf-8", AS_INDEX_LEN, "Cache-Control: no-cache\r\n", 1))
			sock_sendall(r->s, (const char*)AS_INDEX, AS_INDEX_LEN);
		return 1;
	}
	if (!strcmp(p, "/app.js")){
		if (!isGet){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		if (hdr_send(r->s, 200, "text/javascript; charset=utf-8", AS_APPJS_LEN, "Cache-Control: no-cache\r\n", 1))
			sock_sendall(r->s, (const char*)AS_APPJS, AS_APPJS_LEN);
		return 1;
	}
	if (!strcmp(p, "/style.css")){
		if (!isGet){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		if (hdr_send(r->s, 200, "text/css; charset=utf-8", AS_CSS_LEN, "Cache-Control: no-cache\r\n", 1))
			sock_sendall(r->s, (const char*)AS_CSS, AS_CSS_LEN);
		return 1;
	}
	if (!strcmp(p, "/api/info")){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hInfo(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/list")){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hList(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/qr")){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hQR(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/devices")){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hDevices(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/texts")){
		if (GET){ hTextsGet(a, r); return 1; }
		if (POST){ hTextsPost(a, r); return 1; }
		if (DEL){ store_clearTexts(); resp_ok(a, r); return 1; }
		resp_err(a, r, 405, "Method Not Allowed");
		return 1;
	}
	if (!strncmp(p, "/api/text/", 10)){
		if (!DEL){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hTextDelete(a, r, p + 10);
		return 1;
	}
	if (!strcmp(p, "/api/history")){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hHistory(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/history/clear")){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hHistoryClear(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/delete")){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hDelete(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/mkdir")){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hMkdir(a, r);
		return 1;
	}
	if (!strcmp(p, "/api/upload/init")){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hUploadInit(a, r);
		return 1;
	}
	if (!strncmp(p, "/api/upload/chunk/", 18)){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hUploadChunk(a, r, p + 18);
		return 1;
	}
	if (!strncmp(p, "/api/upload/status/", 19)){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hUploadStatus(a, r, p + 19);
		return 1;
	}
	if (!strncmp(p, "/api/upload/complete/", 21)){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hUploadComplete(a, r, p + 21);
		return 1;
	}
	if (!strncmp(p, "/api/upload/abort/", 18)){
		if (!POST){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hUploadAbort(a, r, p + 18);
		return 1;
	}
	if (!strncmp(p, "/d/", 3)){
		if (!isGet){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hDownload(a, r, p + 3);
		return 1;
	}
	if (!strncmp(p, "/zip/", 5)){
		if (!GET){ resp_err(a, r, 405, "Method Not Allowed"); return 1; }
		hZip(a, r, p + 5);
		return 1;
	}
	/* 404（对齐 Go http.NotFound） */
	if (hdr_send(r->s, 404, "text/plain; charset=utf-8", 19, NULL, 1))
		sock_sendall(r->s, "404 page not found\n", 19);
	return 1;
}

static DWORD WINAPI connProc(LPVOID arg){
	SOCKET s = (SOCKET)(UINT_PTR)arg;
	Conn c;
	memset(&c, 0, sizeof(c));
	c.s = s;
	struct sockaddr_in sa;
	int sl = sizeof(sa);
	if (getpeername(s, (struct sockaddr*)&sa, &sl) == 0)
		addr_str((unsigned)sa.sin_addr.s_addr, c.ip);
	int tmo = 20000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
	int alive = 1;
	while (alive){
		Aren a; memset(&a, 0, sizeof(a));
		ParsedReq pr = conn_read(&c, &a);
		if (pr.bad || !pr.rq.method){
			if (pr.bad == 2)
				hdr_send(c.s, 413, "text/plain; charset=utf-8", 0, NULL, 0);
			ar_free(&a);
			break;
		}
		if (!auth_ok(&pr.rq)){
			const char *ex = "WWW-Authenticate: Basic realm=\"FileTra\", charset=\"UTF-8\"\r\n";
			if (hdr_send(c.s, 401, "text/plain; charset=utf-8", strlen("需要访问密码\n"), ex, 1))
				sock_sendall(c.s, "需要访问密码\n", strlen("需要访问密码\n"));
			ar_free(&a);
			continue;
		}
		route(&a, &pr.rq);
		ar_free(&a);
		if (pr.rq.closeAfter) break;
	}
	closesocket(s);
	conn_free(&c);
	return 0;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
static void banner(const char *url){
	const char *LN = "─────────────────────────────────────────";
	printf("\n  FileTra  ·  局域网文件传输\n");
	printf("  %s\n", LN);
	printf("  共享目录 : %s\n", g_rootU8);
	char dd[PWCAP*2];
	{
		Aren a; memset(&a, 0, sizeof(a));
		char *u = w2u(&a, g_dataW);
		strncpy(dd, u, sizeof(dd) - 1);
		dd[sizeof(dd) - 1] = 0;
		ar_free(&a);
	}
	printf("  数据目录 : %s\n", dd);
	printf("  本机地址 : %s\n", url);
	{
		Aren a; memset(&a, 0, sizeof(a));
		char **ips; int n;
		lan_list(&a, &ips, &n);
		for (int i = 1; i < n; i++)
			printf("             http://%s:%d\n", ips[i], g_port);
		ar_free(&a);
	}
	if (g_pass[0]) printf("  访问密码 : 已启用（用户名 %s）\n", g_user);
	printf("  其他设备:浏览器访问上面的地址即可互传\n");
	printf("  %s\n\n", LN);
	fflush(stdout);
}

/* ------------------------------------------------------------------
 * 外置配置文件 filetra.conf（与 exe 同目录）
 * 优先级：配置文件 > 命令行参数 > 内置默认值
 * 格式：UTF-8（可带 BOM），key=value，# 开头整行注释；空值=未设置；
 *       未知键/不合法值打印提示后忽略，其余行为不变
 * ------------------------------------------------------------------ */
#define CONF_MAX  (64*1024)
static char g_confBuf[CONF_MAX + 8];

static int conf_ws(char c){
	return c == ' ' || c == '\t' || c == '\r';
}
static char *conf_lskip(char *s){
	while (*s && conf_ws(*s)) s++;
	return s;
}
static void conf_rstrip(char *s){
	size_t n = strlen(s);
	while (n && conf_ws(s[n-1])) s[--n] = 0;
}
static int conf_eq_ci(const char *a, const char *b){ /* ASCII 不区分大小写比较 */
	while (*a && *b){
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
		a++; b++;
	}
	return *a == 0 && *b == 0;
}
static int conf_int(const char *v, int lo, int hi, int *out){
	if (!v[0]) return 0;
	char *e; long n = strtol(v, &e, 10);
	if (e == v || n < lo || n > hi) return 0;
	while (*e){ if (!conf_ws(*e++)) return 0; }
	*out = (int)n;
	return 1;
}
static int conf_bool(const char *v, int *out){
	if (conf_eq_ci(v, "true") || conf_eq_ci(v, "1") || conf_eq_ci(v, "yes") || conf_eq_ci(v, "on")){ *out = 1; return 1; }
	if (conf_eq_ci(v, "false") || conf_eq_ci(v, "0") || conf_eq_ci(v, "no") || conf_eq_ci(v, "off")){ *out = 0; return 1; }
	return 0;
}

static void conf_apply(const char **dirArg){
	wchar_t exep[520];
	DWORD n = GetModuleFileNameW(NULL, exep, 520);
	if (!n || n >= 520) return;
	wchar_t *sl = wcsrchr(exep, L'\\');
	if (!sl) return;
	sl[1] = 0;
	if (!wccat(exep, 520, L"filetra.conf")) return;

	HANDLE f = CreateFileW(exep, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) return;              /* 没有配置文件 → 一切照旧 */
	DWORD fsz = GetFileSize(f, NULL);
	if (fsz == INVALID_FILE_SIZE || fsz > CONF_MAX){
		CloseHandle(f);
		if (fsz != INVALID_FILE_SIZE) fprintf(stderr, "配置文件过大（上限 64KB），已忽略\n");
		return;
	}
	DWORD rd = 0;
	if (!ReadFile(f, g_confBuf, fsz, &rd, NULL) || rd != fsz){ CloseHandle(f); return; }
	CloseHandle(f);
	g_confBuf[rd] = 0;
	char *base = g_confBuf;
	if (rd >= 3 && (unsigned char)base[0] == 0xEF && (unsigned char)base[1] == 0xBB && (unsigned char)base[2] == 0xBF)
		base += 3;                                      /* 跳过 UTF-8 BOM */

	char *applied[6]; int nApplied = 0;
	char *p = base;
	for (int ln = 1; *p; ln++){
		char *nl = strchr(p, '\n');
		if (nl) *nl = 0;
		char *line = conf_lskip(p);
		conf_rstrip(line);
		if (!*line || *line == '#'){ p = nl ? nl + 1 : p + strlen(p); continue; }
		char *eq = strchr(line, '=');
		if (!eq){
			fprintf(stderr, "配置第 %d 行缺少 '='，已忽略\n", ln);
			p = nl ? nl + 1 : p + strlen(p); continue;
		}
		*eq = 0;                                        /* key 即 line（已去两侧空白） */
		char *key = line;
		char *val = conf_lskip(eq + 1);
		conf_rstrip(val);
		char kn[40];
		if (strlen(key) >= sizeof(kn)){
			fprintf(stderr, "配置第 %d 行键名过长，已忽略\n", ln);
		} else if (!*val){
			/* 空值统一按"未设置"处理（password 留空=不启用，dir 留空=用默认） */
		} else if (conf_eq_ci(key, "port")){
			int pp;
			if (!conf_int(val, 1, 65535, &pp))
				fprintf(stderr, "配置第 %d 行 port 值无效（应为 1~65535），已忽略\n", ln);
			else { g_port = pp; applied[nApplied++] = "port"; }
		} else if (conf_eq_ci(key, "name")){
			if (strlen(val) >= sizeof(g_nameU8) - 1)
				fprintf(stderr, "配置第 %d 行 name 过长，已忽略\n", ln);
			else { strcpy(g_nameU8, val); applied[nApplied++] = "name"; }
		} else if (conf_eq_ci(key, "dir")){
			if (strlen(val) >= 518)
				fprintf(stderr, "配置第 %d 行 dir 过长，已忽略\n", ln);
			else { *dirArg = val; applied[nApplied++] = "dir"; }
		} else if (conf_eq_ci(key, "password")){
			if (strlen(val) >= sizeof(g_pass) - 1)
				fprintf(stderr, "配置第 %d 行 password 过长，已忽略\n", ln);
			else { strcpy(g_pass, val); applied[nApplied++] = "password"; }
		} else if (conf_eq_ci(key, "user")){
			if (strchr(val, ':'))
				fprintf(stderr, "配置第 %d 行 user 不能包含 ':'，已忽略\n", ln);
			else if (strlen(val) >= sizeof(g_user))
				fprintf(stderr, "配置第 %d 行 user 过长，已忽略\n", ln);
			else { strcpy(g_user, val); applied[nApplied++] = "user"; }
		} else if (conf_eq_ci(key, "no-browser")){
			int b;
			if (!conf_bool(val, &b))
				fprintf(stderr, "配置第 %d 行 no-browser 值无效（true/false/1/0/yes/no/on/off），已忽略\n", ln);
			else { g_noBrowser = b; applied[nApplied++] = "no-browser"; }
		} else {
			fprintf(stderr, "配置第 %d 行未识别的键 '%s'，已忽略\n", ln, key);
		}
		p = nl ? nl + 1 : p + strlen(p);
	}
	if (nApplied){
		printf("配置文件 filetra.conf 生效：");
		for (int i = 0; i < nApplied; i++) printf(i ? "、%s" : "%s", applied[i]);
		printf("\n");
		fflush(stdout);
	}
}

static void resolve_paths(const char *dirArg){
	wchar_t arg[520];
	MultiByteToWideChar(CP_UTF8, 0, dirArg, -1, arg, 520);
	wchar_t *full = g_rootW;
	DWORD n = GetFullPathNameW(arg, PWCAP, full, NULL);
	if (!n || n >= PWCAP){ fprintf(stderr, "无法解析共享目录\n"); exit(1); }
	FINFO fi;
	if (!wstat(full, &fi) || !fi.isDir){
		fprintf(stderr, "共享目录不存在或不是目录\n");
		exit(1);
	}
	char *u = w2u_m(full);
	strncpy(g_rootU8, u, sizeof(g_rootU8) - 1);
	g_rootU8[sizeof(g_rootU8) - 1] = 0;
	free(u);
	/* 数据目录 */
	wchar_t base[520];
	DWORD al = GetEnvironmentVariableW(L"APPDATA", base, 520);
	if (!al || al >= 520){
		DWORD tl = GetTempPathW(520, base);
		if (tl && base[tl-1] == L'\\') base[tl-1] = 0;
	}
	wcscpy(g_dataW, base);
	if (!wccat(g_dataW, PWCAP, L"\\filetra")){
		fprintf(stderr, "数据目录路径过长\n");
		exit(1);
	}
	CreateDirectoryW(g_dataW, NULL);
	FINFO di;
	if (!wstat(g_dataW, &di) || !di.isDir){
		fprintf(stderr, "无法创建数据目录\n");
		exit(1);
	}
	wcscpy(g_histPathW, g_dataW);
	wccat(g_histPathW, PWCAP, L"\\history.json");
	wcscpy(g_textsPathW, g_dataW);
	wccat(g_textsPathW, PWCAP, L"\\texts.json");
}

static int port_probe(int want){
	/* 从 want 起找可用端口；成功者直接作监听 socket 返回 */
	for (int i = 0; i < 100; i++){
		int p = want + i;
		if (p < 1 || p > 65535) break;
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET) return -1;
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = INADDR_ANY;
		a.sin_port = htons((unsigned short)p);
		if (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(s, 64) == 0)
			return (int)(INT_PTR)s;
		closesocket(s);
	}
	return -1;
}

int main(int argc, char **argv){
	SetConsoleOutputCP(65001);
	WSADATA wd;
	if (WSAStartup(MAKEWORD(2, 2), &wd) != 0){
		fprintf(stderr, "WSAStartup 失败\n");
		return 1;
	}
	crc32_init();

	const char *dirArg = ".";
	randHex(g_instance, 6);
	for (int i = 1; i < argc; i++){
		const char *k = argv[i];
		const char *val = NULL;
		char key[64];
		strncpy(key, k, 63);
		key[63] = 0;
		char *eq = strchr(key, '=');
		if (eq){ *eq = 0; val = eq + 1; }
		if (!strcmp(key, "-port") || !strcmp(key, "--port")){
			if (!val && i + 1 < argc) val = argv[++i];
			if (val) g_port = atoi(val);
		} else if (!strcmp(key, "-dir") || !strcmp(key, "--dir")){
			if (!val && i + 1 < argc) val = argv[++i];
			if (val) dirArg = val;
		} else if (!strcmp(key, "-name") || !strcmp(key, "--name")){
			if (!val && i + 1 < argc) val = argv[++i];
			if (val && val[0]) strncpy(g_nameU8, val, 255);
		} else if (!strcmp(key, "-password") || !strcmp(key, "--password")){
			if (!val && i + 1 < argc) val = argv[++i];
			if (val) strncpy(g_pass, val, 127);
		} else if (!strcmp(key, "-user") || !strcmp(key, "--user")){
			if (!val && i + 1 < argc) val = argv[++i];
			if (val && val[0] && !strchr(val, ':') && strlen(val) < sizeof(g_user)){
				strncpy(g_user, val, sizeof(g_user) - 1);
				g_user[sizeof(g_user) - 1] = 0;
			}
		} else if (!strcmp(key, "-no-browser") || !strcmp(key, "--no-browser")){
			g_noBrowser = 1;
		}
	}
	g_nameU8[255] = 0;
	conf_apply(&dirArg);
	resolve_paths(dirArg);
	if (!g_nameU8[0]){
		wchar_t cn[256];
		DWORD cnl = 256;
		if (GetComputerNameW(cn, &cnl)){
			char *u = w2u_m(cn);
			strncpy(g_nameU8, u, sizeof(g_nameU8) - 1);
			g_nameU8[sizeof(g_nameU8) - 1] = 0;
			free(u);
		}
	}
	if (!g_nameU8[0]) strcpy(g_nameU8, "未命名设备");
	{
		int wl = MultiByteToWideChar(CP_UTF8, 0, g_nameU8, -1, g_nameW, 256);
		(void)wl;
	}

	store_loadAll();
	InitializeCriticalSection(&g_sesLock);

	g_listen = (SOCKET)(INT_PTR)port_probe(g_port);
	if (g_listen == INVALID_SOCKET){
		fprintf(stderr, "没有找到可用端口（从 %d 开始尝试）\n", g_port);
		return 1;
	}
	SOCKET svc = g_listen;
	struct sockaddr_in sa;
	int sl = sizeof(sa);
	if (getsockname(svc, (struct sockaddr*)&sa, &sl) == 0)
		g_port = ntohs(sa.sin_port);

	CreateThread(NULL, 0, janitorProc, NULL, 0, NULL);
	startDiscovery();

	char url[128];
	primaryURL(url, sizeof(url));
	banner(url);
	if (!g_noBrowser){
		wchar_t uw[256];
		MultiByteToWideChar(CP_UTF8, 0, url, -1, uw, 256);
		SHELLEXECUTEINFOW si;
		memset(&si, 0, sizeof(si));
		si.cbSize = sizeof(si);
		si.lpVerb = L"open";
		si.lpFile = uw;
		si.nShow = SW_SHOWNORMAL;
		ShellExecuteExW(&si);
	}
	for (;;){
		struct sockaddr_in ca;
		int cl = sizeof(ca);
		SOCKET cs = accept(svc, (struct sockaddr*)&ca, &cl);
		if (cs == INVALID_SOCKET){ Sleep(20); continue; }
		HANDLE h = CreateThread(NULL, 0, connProc, (LPVOID)(UINT_PTR)cs, 0, NULL);
		if (h) CloseHandle(h);
	}
	return 0;
}
