"""Banque TONE3000 : chercher et télécharger des captures NAM / IR dans ToneForge\\models, \\pedals et \\irs."""
import json
import os
import re
import threading
import time
import tkinter as tk
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from tkinter import messagebox, ttk

API = 'https://www.tone3000.com/api/v1'
CLIENT_ID = 't3k_pub_-Ohz1QoClePkRMx4WLNh0NMFvPalbJcd'   # clé publique (publishable), sans risque
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOKEN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 't3k_token.json')
# libellé -> (gears, format)
GEARS = {'Tout (NAM)': ('', 'nam'), 'Ampli': ('amp', 'nam'), 'Ampli + baffle': ('amp-cab', 'nam'),
         'Pédale': ('pedal', 'nam'), 'Rack / outboard': ('outboard', 'nam'), 'IR baffle': ('', 'ir')}


class ApiError(Exception):
    pass


def _req(method, path, data=None, token=None, raw=False):
    url = path if path.startswith('http') else API + path
    body = urllib.parse.urlencode(data).encode() if data and method == 'POST' else None
    if data and method == 'GET':
        url += '?' + urllib.parse.urlencode({k: v for k, v in data.items() if v not in ('', None)})
    r = urllib.request.Request(url, body, method=method, headers={'User-Agent': 'ToneForge/0.1'})
    if body:
        r.add_header('Content-Type', 'application/x-www-form-urlencoded')
    if token:
        r.add_header('Authorization', 'Bearer ' + token)
    try:
        with urllib.request.urlopen(r, timeout=60) as resp:
            out = resp.read()
    except urllib.error.HTTPError as e:
        txt = e.read().decode('utf-8', 'replace')
        try:
            j = json.loads(txt)
        except ValueError:
            j = {}
        err = ApiError(j.get('error_description') or j.get('error') or j.get('message') or 'HTTP %d' % e.code)
        err.code, err.error = e.code, j.get('error')
        raise err from None
    return out if raw else json.loads(out)


class Session:
    def __init__(self):
        self.tok = {}
        if os.path.exists(TOKEN_FILE):
            with open(TOKEN_FILE, encoding='utf-8') as f:
                self.tok = json.load(f)

    def _save(self, t):
        t['obtained'] = time.time()
        self.tok = t
        with open(TOKEN_FILE, 'w', encoding='utf-8') as f:
            json.dump(t, f)

    @property
    def ok(self):
        return bool(self.tok.get('access_token'))

    def device_start(self):
        return _req('POST', '/oauth/device_authorization', {'client_id': CLIENT_ID})

    def device_poll(self, d, cancelled):
        interval = int(d.get('interval', 5))
        end = time.time() + int(d.get('expires_in', 600))
        while time.time() < end and not cancelled():
            time.sleep(interval)
            try:
                self._save(_req('POST', '/oauth/token', {
                    'grant_type': 'urn:ietf:params:oauth:grant-type:device_code',
                    'device_code': d['device_code'], 'client_id': CLIENT_ID}))
                return True
            except ApiError as e:
                if e.error == 'authorization_pending':
                    continue
                if e.error == 'slow_down':
                    interval += 5
                    continue
                raise
        return False

    def _refresh(self):
        self._save(_req('POST', '/oauth/token', {'grant_type': 'refresh_token', 'client_id': CLIENT_ID,
                                                 'refresh_token': self.tok.get('refresh_token', '')}))

    def call(self, method, path, data=None, raw=False):
        if not self.ok:
            raise ApiError('Connecte-toi d\'abord.')
        if time.time() > self.tok.get('obtained', 0) + self.tok.get('expires_in', 3600) - 60:
            self._refresh()
        try:
            return _req(method, path, data, self.tok['access_token'], raw)
        except ApiError as e:
            if e.code != 401:
                raise
            self._refresh()
            return _req(method, path, data, self.tok['access_token'], raw)


def safe(s):
    return re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', s).strip(' .')[:80] or 'sans_nom'


def download_tone(sess, tone, log):
    """Télécharge toutes les captures d'un son. Renvoie le nombre de fichiers écrits."""
    is_ir = tone.get('format') == 'ir'
    sub = 'irs' if is_ir else 'pedals' if tone.get('gear') == 'pedal' else 'models'
    folder = os.path.join(ROOT, sub, safe(tone['title']))
    os.makedirs(folder, exist_ok=True)
    n, page = 0, 1
    while True:
        r = sess.call('GET', '/models', {'tone_id': tone['id'], 'page': page, 'page_size': 100})
        for m in r['data']:
            ext = os.path.splitext(urllib.parse.urlparse(m['model_url']).path)[1] or ('.wav' if is_ir else '.nam')
            dest = os.path.join(folder, safe(m['name']) + ext)
            if os.path.exists(dest):
                continue
            data = sess.call('GET', m['model_url'], raw=True)
            with open(dest, 'wb') as f:
                f.write(data)
            n += 1
            log('  %s' % os.path.basename(dest))
        if page >= r.get('total_pages', 1):
            return n
        page += 1


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('Banque TONE3000 → ToneForge')
        self.geometry('900x600')
        self.sess = Session()
        self.tones, self.page, self.pages = [], 1, 1
        self.cancel_login = False

        top = ttk.Frame(self, padding=8)
        top.pack(fill='x')
        self.login_btn = ttk.Button(top, command=self.login)
        self.login_btn.pack(side='left')
        ttk.Button(top, text='Importer mes favoris', command=self.favorites).pack(side='left', padx=8)
        self.status = ttk.Label(top, text='')
        self.status.pack(side='left', padx=8)

        bar = ttk.Frame(self, padding=(8, 0))
        bar.pack(fill='x')
        self.q = ttk.Entry(bar, width=40)
        self.q.pack(side='left')
        self.q.bind('<Return>', lambda e: self.search(1))
        self.gear = ttk.Combobox(bar, values=list(GEARS), state='readonly', width=14)
        self.gear.set('Tout (NAM)')
        self.gear.pack(side='left', padx=6)
        self.sort = ttk.Combobox(bar, values=['best-match', 'downloads-all-time', 'trending', 'newest'],
                                 state='readonly', width=18)
        self.sort.set('best-match')
        self.sort.pack(side='left')
        ttk.Button(bar, text='Chercher', command=lambda: self.search(1)).pack(side='left', padx=6)
        ttk.Button(bar, text='◀', width=3, command=lambda: self.search(self.page - 1)).pack(side='left')
        ttk.Button(bar, text='▶', width=3, command=lambda: self.search(self.page + 1)).pack(side='left')

        cols = ('title', 'by', 'format', 'models', 'dl')
        self.tree = ttk.Treeview(self, columns=cols, show='headings', selectmode='extended')
        for c, t, w in zip(cols, ('Son', 'Auteur', 'Format', 'Fichiers', 'Téléch.'), (420, 150, 70, 70, 80)):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor='w' if c in ('title', 'by') else 'center')
        self.tree.pack(fill='both', expand=True, padx=8, pady=6)

        bot = ttk.Frame(self, padding=8)
        bot.pack(fill='x')
        ttk.Button(bot, text='Télécharger la sélection', command=self.download_sel).pack(side='left')
        ttk.Button(bot, text='Ouvrir le dossier models', command=lambda: os.startfile(os.path.join(ROOT, 'models'))
                   ).pack(side='left', padx=8)
        self.logbox = tk.Text(self, height=8, state='disabled')
        self.logbox.pack(fill='x', padx=8, pady=(0, 8))
        self.refresh_login()

    # ---------- utilitaires ----------
    def log(self, s):
        def w():
            self.logbox.configure(state='normal')
            self.logbox.insert('end', s + '\n')
            self.logbox.see('end')
            self.logbox.configure(state='disabled')
        self.after(0, w)

    def bg(self, fn):
        def run():
            try:
                fn()
            except Exception as e:   # erreurs réseau / API -> fenêtre
                self.after(0, lambda: messagebox.showerror('TONE3000', str(e)))
        threading.Thread(target=run, daemon=True).start()

    def refresh_login(self):
        self.login_btn.configure(text='Reconnecter' if self.sess.ok else 'Se connecter à TONE3000')
        self.status.configure(text='Connecté' if self.sess.ok else 'Non connecté')

    # ---------- actions ----------
    def login(self):
        def go():
            d = self.sess.device_start()
            url = d.get('verification_uri_complete') or d['verification_uri']
            self.after(0, lambda: self.status.configure(text='Code : %s  (page ouverte dans le navigateur)' % d['user_code']))
            webbrowser.open(url)
            ok = self.sess.device_poll(d, lambda: self.cancel_login)
            self.after(0, self.refresh_login)
            self.log('Connexion réussie.' if ok else 'Connexion expirée, recommence.')
        self.bg(go)

    def show(self, tones):
        self.tones = tones
        self.tree.delete(*self.tree.get_children())
        for i, t in enumerate(tones):
            self.tree.insert('', 'end', iid=str(i), values=(
                t['title'], (t.get('user') or {}).get('username', ''), t.get('format', ''),
                t.get('models_count', ''), t.get('downloads_count', '')))

    def search(self, page):
        if page < 1 or page > self.pages:
            return
        def go():
            r = self.sess.call('GET', '/tones/search', {
                'query': self.q.get().strip(), 'page': page, 'page_size': 25,
                'gears': GEARS[self.gear.get()][0], 'format': GEARS[self.gear.get()][1], 'sort': self.sort.get() if self.q.get().strip() else
                ('downloads-all-time' if self.sort.get() == 'best-match' else self.sort.get())})
            self.page, self.pages = r.get('page', page), max(1, r.get('total_pages', 1))
            self.after(0, lambda: (self.show(r['data']),
                                   self.status.configure(text='Page %d / %d  (%d sons)' % (self.page, self.pages, r.get('total', 0)))))
        self.bg(go)

    def favorites(self):
        def go():
            tones, page = [], 1
            while True:
                r = self.sess.call('GET', '/tones/favorited', {'page': page, 'page_size': 100})
                tones += r['data']
                if page >= r.get('total_pages', 1):
                    break
                page += 1
            self.after(0, lambda: self.show(tones))
            self.log('%d favoris trouvés.' % len(tones))
            self._download(tones)
        self.bg(go)

    def download_sel(self):
        sel = [self.tones[int(i)] for i in self.tree.selection()]
        if not sel:
            messagebox.showinfo('TONE3000', 'Sélectionne un ou plusieurs sons (Ctrl/Maj + clic).')
            return
        self.bg(lambda: self._download(sel))

    def _download(self, tones):
        total = 0
        for t in tones:
            self.log('%s :' % t['title'])
            total += download_tone(self.sess, t, self.log)
        self.log('Terminé : %d nouveaux fichiers.' % total)


if __name__ == '__main__':
    App().mainloop()
