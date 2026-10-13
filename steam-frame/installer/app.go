package main

// The installer's state and work: step 1 (find the Frame, check Developer Mode, sign in, check the Frame is ready) and step 2
// (install, update or uninstall). The page polls GET /api/state and starts the work with POST requests; every API request
// carries the random token of the page's address, so other web pages cannot drive the installer.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	repo         = "SPD13/CitraVR-steam-frame"
	libraryName  = "Citra VR - Steam Frame Edition"
	defaultUser  = "steamos"
	defaultHost  = "frame.local"
	apkAsset     = "CitraVR-steam-frame.apk"
	neededFreeKB = 300_000 // the download, the APK's copy in the title folder, and Lepton's install of it
	// install.sh's title folder and data folder (GAMEID and DATA_DIR there); VERSION is written by this installer, and by
	// install.sh in releases after v1.1.1
	titleAPK    = "$HOME/devkit-game/citravr/CitraVR.apk"
	versionFile = "$HOME/.local/share/citravr/VERSION"
)

type checkItem struct {
	ID     string   `json:"id"`
	Title  string   `json:"title"`
	Status string   `json:"status"` // idle, running, ok, fail, wait (needs the user: the password)
	Detail string   `json:"detail"`
	Help   []string `json:"help,omitempty"`
	Action string   `json:"action,omitempty"` // a button the page shows with the help: "trust-key"
}

type releaseInfo struct {
	Loading bool   `json:"loading"`
	Error   string `json:"error,omitempty"`
	Version string `json:"version,omitempty"` // the release's tag, e.g. v1.1.1
	Date    string `json:"date,omitempty"`
	Size    int64  `json:"size,omitempty"`   // the APK's
	Pinned  bool   `json:"pinned,omitempty"` // --release
}

type installedInfo struct {
	Known   bool   `json:"known"` // the Frame was checked
	Present bool   `json:"present"`
	Version string `json:"version,omitempty"`
}

type jobLine struct {
	Text string `json:"text"`
	Kind string `json:"kind"` // step, note, error
}

type jobState struct {
	Kind    string    `json:"kind"` // install, uninstall
	Running bool      `json:"running"`
	Failed  bool      `json:"failed"`
	Message string    `json:"message,omitempty"`
	Lines   []jobLine `json:"lines"`
}

type appState struct {
	Title       string        `json:"title"`
	Address     string        `json:"address"`
	Frames      []foundFrame  `json:"frames"`
	Discovering bool          `json:"discovering"`
	Checking    bool          `json:"checking"`
	Checks      []checkItem   `json:"checks"`
	Ready       bool          `json:"ready"`
	Installed   installedInfo `json:"installed"`
	Latest      releaseInfo   `json:"latest"`
	Job         *jobState     `json:"job"`
	LibraryName string        `json:"libraryName"`
	Version     string        `json:"version"`
	LogPath     string        `json:"logPath"`
	Browser     bool          `json:"browser"`
}

type app struct {
	token   string
	release string // --release, or "" for the latest

	mu       sync.Mutex
	state    appState
	conn     *frameConnection
	user     string
	ip       string
	keyName  string
	password string
	busy     bool

	lastPoll atomic.Int64
	quit     chan struct{}
	quitOnce sync.Once
}

func newApp(token, release string) *app {
	a := &app{token: token, release: release, quit: make(chan struct{})}
	a.state = appState{Title: appTitle, LibraryName: libraryName, Version: version, LogPath: logPath, Checks: freshChecks(), Latest: releaseInfo{Loading: true}}
	return a
}

func freshChecks() []checkItem {
	return []checkItem{
		{ID: "find", Title: "Frame found on the network", Status: "idle"},
		{ID: "devmode", Title: "Developer Mode is on", Status: "idle"},
		{ID: "login", Title: "Signed in to the Frame", Status: "idle"},
		{ID: "ready", Title: "Frame ready for CitraVR", Status: "idle"},
	}
}

func (a *app) update(change func(*appState)) {
	a.mu.Lock()
	defer a.mu.Unlock()
	change(&a.state)
}

func (a *app) setCheck(id, status, detail string, help ...string) {
	logf("check %s: %s: %s", id, status, detail)
	a.update(func(s *appState) {
		for i := range s.Checks {
			if s.Checks[i].ID == id {
				s.Checks[i] = checkItem{ID: id, Title: s.Checks[i].Title, Status: status, Detail: detail, Help: help}
			}
		}
	})
}

// start: look up the latest release, find the Frames on the network, and check the likely one without a password (an ssh
// key may be enough)
func (a *app) start() {
	go a.fetchRelease()
	a.update(func(s *appState) { s.Discovering = true })
	frames := discoverFrames(2 * time.Second)
	logf("found on the network: %v", frames)
	last := loadSettings().LastAddress
	address := last
	if address == "" || (len(frames) > 0 && !matchesFrame(last, frames)) {
		address = defaultHost
		if len(frames) > 0 {
			address = frameAddress(frames[0])
		}
	}
	a.update(func(s *appState) {
		s.Discovering = false
		s.Frames = frames
		if s.Address == "" {
			s.Address = address
		}
	})
	a.mu.Lock()
	address = a.state.Address
	a.mu.Unlock()
	a.runChecks(address, "")
}

func frameAddress(f foundFrame) string {
	if f.Host != "" {
		return f.Host
	}
	return f.Address
}

func matchesFrame(address string, frames []foundFrame) bool {
	_, host := splitUser(address)
	for _, f := range frames {
		if strings.EqualFold(host, f.Host) || host == f.Address {
			return true
		}
	}
	return false
}

func splitUser(address string) (user, host string) {
	address = strings.TrimSpace(address)
	if i := strings.LastIndex(address, "@"); i >= 0 {
		return address[:i], address[i+1:]
	}
	return defaultUser, address
}

// runChecks is step 1. A password of "" tries the user's ssh keys only, and asks for the password when they are refused.
func (a *app) runChecks(address, password string) {
	a.mu.Lock()
	if a.busy {
		a.mu.Unlock()
		return
	}
	a.busy = true
	if a.conn != nil {
		a.conn.close()
		a.conn = nil
	}
	a.state.Checking = true
	a.state.Ready = false
	a.state.Address = strings.TrimSpace(address)
	a.state.Checks = freshChecks()
	a.state.Installed = installedInfo{}
	if a.state.Job != nil && !a.state.Job.Running {
		a.state.Job = nil
	}
	frames := a.state.Frames
	a.mu.Unlock()
	defer a.update(func(s *appState) { s.Checking = false })
	defer func() { a.mu.Lock(); a.busy = false; a.mu.Unlock() }()

	user, host := splitUser(address)
	if host == "" {
		a.setCheck("find", "fail", "No address", "Type the Frame's name (frame.local) or its IP address above, then select Connect.")
		return
	}

	// 1. The Frame on the network
	a.setCheck("find", "running", "Looking for "+host+"...")
	ip := ""
	if parsed := net.ParseIP(host); parsed != nil {
		ip = parsed.String()
		a.setCheck("find", "ok", "IP address "+ip)
	} else {
		for _, f := range frames {
			if strings.EqualFold(f.Host, host) || strings.EqualFold(f.Name, host) {
				ip = f.Address
			}
		}
		if ip == "" {
			if found, ok := resolveHost(host, 3*time.Second); ok {
				ip = found
			}
		}
		if ip == "" {
			// The list may have been made before the Frame woke up: look again
			for _, f := range discoverFrames(2 * time.Second) {
				if strings.EqualFold(f.Host, host) || strings.EqualFold(f.Name, host) {
					ip = f.Address
				}
			}
		}
		if ip == "" {
			a.setCheck("find", "fail", host+" is not found on the network", append([]string{
				"Put the headset on so it is awake: it leaves the network when it sleeps.",
				"The Frame and this computer must be on the same network (the same Wi-Fi or router). A guest network often keeps devices apart.",
				"Developer Mode must be on: on the Frame, **Settings > System > Enable Developer Mode**.",
				"Or type the Frame's IP address above, shown on the Frame in **Settings > Network** (select the network), for example 192.168.1.42. Then select **Connect**."},
				localNetworkHelp()...)...)
			return
		}
		a.setCheck("find", "ok", fmt.Sprintf("%s (%s)", host, ip))
	}

	// 2. Developer Mode: the Frame accepts ssh connections
	a.setCheck("devmode", "running", "Connecting to "+ip+"...")
	probe, err := net.DialTimeout("tcp", net.JoinHostPort(ip, "22"), 8*time.Second)
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "refused") {
			a.setCheck("devmode", "fail", "The Frame does not accept connections",
				"Turn Developer Mode on: on the Frame, **Settings > System > Enable Developer Mode**.",
				"Then, in the **Developer** section that appears, choose **Set User Password** and pick a password.",
				"If "+ip+" is not the Frame's address, type the right one above.")
		} else {
			a.setCheck("devmode", "fail", "No answer from "+ip, append([]string{
				"Put the headset on so it is awake: it leaves the network when it sleeps.",
				"Check that " + ip + " is the Frame's address (on the Frame: **Settings > Network**, select the network).",
				"Some networks keep devices apart (guest Wi-Fi, \"client isolation\"): use the main network."},
				localNetworkHelp()...)...)
		}
		return
	}
	probe.Close()
	a.setCheck("devmode", "ok", "ssh is open on "+ip)

	// 3. Sign in
	a.setCheck("login", "running", "Signing in as "+user+"...")
	keyName := strings.ToLower(host)
	conn, err := dialFrame(user, ip, keyName, password)
	if err != nil {
		var changed *errHostKeyChanged
		switch {
		case errors.As(err, &changed):
			a.update(func(s *appState) {
				for i := range s.Checks {
					if s.Checks[i].ID == "login" {
						s.Checks[i].Status = "fail"
						s.Checks[i].Detail = "The Frame's identity has changed"
						s.Checks[i].Help = []string{
							"The Frame does not have the security key it had when this computer last connected to it. This is expected after the Frame was reset or reinstalled.",
							"If you did not reset it, check that the address is really your Frame's. Otherwise, trust its new key.",
						}
						s.Checks[i].Action = "trust-key"
					}
				}
			})
		case strings.Contains(err.Error(), "unable to authenticate") && password == "":
			a.setCheck("login", "wait", "Type the Frame's password, then select Connect",
				"The password is the one set on the Frame in Developer Mode: **Settings > Developer > Set User Password**. Set one there if you have not yet.")
		case strings.Contains(err.Error(), "unable to authenticate"):
			a.setCheck("login", "fail", "The password was refused",
				"Type the password set on the Frame in **Settings > Developer > Set User Password** (it is case sensitive).",
				"Forgot it? Set a new one there, then select **Connect** again.")
		default:
			a.setCheck("login", "fail", "Cannot sign in: "+err.Error(),
				"Put the headset on so it stays awake, then select **Connect** again.")
		}
		return
	}
	a.mu.Lock()
	a.conn, a.user, a.ip, a.keyName, a.password = conn, user, ip, keyName, password
	a.mu.Unlock()
	updateSettings(func(s *settings) { s.LastAddress = address })
	a.setCheck("login", "ok", "Signed in as "+user)

	// 4. Ready: a Steam Frame, Steam running, the tools install.sh uses, room for CitraVR
	a.checkReady(conn)
}

func (a *app) checkReady(conn *frameConnection) {
	a.setCheck("ready", "running", "Checking the Frame...")
	script := `echo "arch=$(uname -m)"
if pgrep -x steam > /dev/null; then echo steam=1; else echo steam=0; fi
echo "free=$(df -Pk "$HOME" | awk 'NR==2 {print $4}')"
for tool in python3 curl; do if command -v $tool > /dev/null; then echo $tool=1; else echo $tool=0; fi; done
if [ -f "` + titleAPK + `" ]; then echo installed=1; else echo installed=0; fi
if [ -f "` + versionFile + `" ]; then echo "version=$(head -c 64 "` + versionFile + `")"; fi
`
	out, err := conn.output("sh -s", strings.NewReader(script))
	if err != nil {
		a.setCheck("ready", "fail", "Cannot check the Frame: "+err.Error(), "Put the headset on so it stays awake, then select **Check again**.")
		return
	}
	values := map[string]string{}
	for _, line := range strings.Split(out, "\n") {
		if key, value, ok := strings.Cut(strings.TrimSpace(line), "="); ok {
			values[key] = value
		}
	}
	free, _ := strconv.ParseInt(values["free"], 10, 64)
	installed := installedInfo{Known: true, Present: values["installed"] == "1", Version: values["version"]}
	if !installed.Present {
		installed.Version = ""
	}
	a.update(func(s *appState) { s.Installed = installed })
	switch {
	case values["arch"] != "aarch64":
		a.setCheck("ready", "fail", "This is not a Steam Frame (a "+values["arch"]+" computer)", "Check the address: it must be the Frame's.")
	case values["python3"] != "1" || values["curl"] != "1":
		a.setCheck("ready", "fail", "Python or curl is missing on this device", "SteamOS on the Frame includes both: check that the address is the Frame's.")
	case values["steam"] != "1":
		a.setCheck("ready", "fail", "Steam is not running on the Frame",
			"Put the headset on so it starts in its normal mode (Steam starts with it), then select **Check again**.")
	case free < neededFreeKB:
		a.setCheck("ready", "fail", fmt.Sprintf("Not enough free space: %s free, %s needed", formatMB(free*1024), formatMB(neededFreeKB*1024)),
			"Uninstall a game or delete files on the Frame, then select **Check again**.")
	default:
		a.setCheck("ready", "ok", fmt.Sprintf("Steam is running, %s free", formatSpace(free*1024)))
		a.update(func(s *appState) { s.Ready = true })
	}
}

// localNetworkHelp: macOS blocks an app's local network connections until it is allowed in its privacy settings (it asks on
// the first launch)
func localNetworkHelp() []string {
	if runtime.GOOS != "darwin" {
		return nil
	}
	return []string{"On this Mac, the installer must be allowed to reach the local network: turn it on in **System Settings > Privacy & Security > Local Network**, then select **Check again**."}
}

func formatSpace(bytes int64) string {
	if bytes >= 10e9 {
		return fmt.Sprintf("%.0f GB", float64(bytes)/1e9)
	}
	return fmt.Sprintf("%.1f GB", float64(bytes)/1e9)
}

// fetchRelease reads the release to install on GitHub (the latest, or --release): its tag, date and APK size
func (a *app) fetchRelease() {
	pinned := a.release != ""
	a.update(func(s *appState) { s.Latest = releaseInfo{Loading: true, Pinned: pinned} })
	info := func() releaseInfo {
		api := "https://api.github.com/repos/" + repo + "/releases/latest"
		if pinned {
			api = "https://api.github.com/repos/" + repo + "/releases/tags/" + a.release
		}
		request, _ := http.NewRequest("GET", api, nil)
		request.Header.Set("Accept", "application/vnd.github+json")
		request.Header.Set("User-Agent", "citravr-frame-installer")
		client := &http.Client{Timeout: 20 * time.Second}
		response, err := client.Do(request)
		if err != nil {
			return releaseInfo{Error: "Cannot reach GitHub to find the latest version. Check this computer's internet connection."}
		}
		defer response.Body.Close()
		if pinned && response.StatusCode == 404 {
			return releaseInfo{Error: "There is no release " + a.release + " on GitHub."}
		}
		if response.StatusCode != 200 {
			return releaseInfo{Error: fmt.Sprintf("GitHub did not give the latest version (%s). Try again in a few minutes.", response.Status)}
		}
		var release struct {
			TagName     string    `json:"tag_name"`
			PublishedAt time.Time `json:"published_at"`
			Assets      []struct {
				Name string `json:"name"`
				Size int64  `json:"size"`
			} `json:"assets"`
		}
		if err := json.NewDecoder(response.Body).Decode(&release); err != nil {
			return releaseInfo{Error: "GitHub's answer could not be read: " + err.Error()}
		}
		info := releaseInfo{Version: release.TagName, Date: release.PublishedAt.Local().Format("2 January 2006"), Pinned: pinned}
		hasInstaller := false
		for _, asset := range release.Assets {
			switch asset.Name {
			case apkAsset:
				info.Size = asset.Size
			case "install.sh":
				hasInstaller = true
			}
		}
		if info.Size == 0 || !hasInstaller {
			return releaseInfo{Error: "The release " + release.TagName + " on GitHub has no APK or install.sh."}
		}
		return info
	}()
	logf("release: %s %s", info.Version, info.Error)
	a.update(func(s *appState) { s.Latest = info })
}

// connection gives the open connection, signing in again (same password) when the Frame dropped it, for instance after a sleep
func (a *app) connection() (*frameConnection, error) {
	a.mu.Lock()
	conn, user, ip, keyName, password := a.conn, a.user, a.ip, a.keyName, a.password
	a.mu.Unlock()
	if conn != nil && conn.alive() {
		return conn, nil
	}
	if ip == "" {
		return nil, errors.New("not connected")
	}
	conn, err := dialFrame(user, ip, keyName, password)
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	a.conn = conn
	a.mu.Unlock()
	return conn, nil
}

// beginJob marks the installer busy with a job, or says why it cannot start
func (a *app) beginJob(kind string) (*jobState, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.busy {
		return nil, errors.New("busy")
	}
	if !a.state.Ready {
		return nil, errors.New("the Frame is not ready: finish step 1")
	}
	a.busy = true
	job := &jobState{Kind: kind, Running: true}
	a.state.Job = job
	return job, nil
}

func (a *app) endJob(job *jobState, failure string) {
	a.update(func(s *appState) {
		job.Running = false
		logf("%s finished: %s", job.Kind, failure)
		if failure != "" {
			job.Failed = true
			job.Message = failure
		}
	})
	a.mu.Lock()
	a.busy = false
	a.mu.Unlock()
	// What is on the Frame now
	if conn, err := a.connection(); err == nil {
		a.checkReady(conn)
	}
}

// addLine sorts what install.sh prints into the page's steps: its first line ("Downloading CitraVR (v1.1.1)..."), the
// registration with Steam and its last line ("Done: ...") start steps, "error: <why>" is the reason it stopped, and the rest
// are notes under the current step
func (a *app) addLine(job *jobState, text string, isError bool) {
	text = strings.TrimSpace(text)
	if text == "" {
		return
	}
	var lines []jobLine
	switch {
	case strings.HasPrefix(text, "==> "):
		lines = []jobLine{{Text: strings.TrimPrefix(text, "==> "), Kind: "step"}}
	case strings.HasPrefix(text, "Downloading CitraVR"):
		lines = []jobLine{{Text: strings.TrimSuffix(text, "..."), Kind: "step"}}
	case strings.HasPrefix(text, "registered as"):
		lines = []jobLine{{Text: "Adding CitraVR to the Steam library", Kind: "step"}, {Text: text, Kind: "note"}}
	case strings.HasPrefix(text, "Done: "):
		lines = []jobLine{{Text: "Done", Kind: "step"}, {Text: strings.TrimPrefix(text, "Done: "), Kind: "note"}}
	case strings.HasPrefix(text, "error: "):
		lines = []jobLine{{Text: strings.TrimPrefix(text, "error: "), Kind: "error"}}
	case isError && (strings.Contains(text, "Error") || strings.Contains(text, "Traceback") || strings.HasPrefix(text, "curl: ")):
		lines = []jobLine{{Text: text, Kind: "error"}}
	default:
		lines = []jobLine{{Text: text, Kind: "note"}}
	}
	for _, line := range lines {
		logf("%s %s: %s", job.Kind, line.Kind, line.Text)
	}
	a.update(func(s *appState) { job.Lines = append(job.Lines, lines...) })
}

func lostConnectionMessage(err error) string {
	return "The connection to the Frame was lost (" + err.Error() + "). Put the headset on so it stays awake, then install again: it is always safe."
}

// runInstaller has the Frame download a release's install.sh from GitHub and run it with args, passing on what it prints
func (a *app) runInstaller(conn *frameConnection, job *jobState, url, args string) error {
	command := `set -e; f=$(mktemp); trap 'rm -f "$f"' EXIT
curl -fsSL --retry 3 -o "$f" ` + shellQuote(url) + ` || { echo "error: the Frame could not download the installer from GitHub. Check that it is connected to the internet." >&2; exit 1; }
bash "$f" ` + args
	return conn.stream(command, nil, func(text string, isError bool) { a.addLine(job, text, isError) })
}

// install is step 2: the release's own install.sh runs on the Frame, which downloads the release from GitHub, checks it
// against its SHA256SUMS and adds CitraVR to the Steam library (or updates it)
func (a *app) install() {
	job, err := a.beginJob("install")
	if err != nil {
		return
	}
	a.mu.Lock()
	release := a.state.Latest
	a.mu.Unlock()
	if release.Error != "" || release.Version == "" {
		a.endJob(job, "CitraVR cannot be downloaded: "+release.Error)
		return
	}
	conn, err := a.connection()
	if err != nil {
		a.endJob(job, lostConnectionMessage(err))
		return
	}
	tag := shellQuote(release.Version)
	url := "https://github.com/" + repo + "/releases/download/" + release.Version + "/install.sh"
	err = a.runInstaller(conn, job, url, "--version "+tag+`; printf '%s\n' `+tag+` > "`+versionFile+`"`)
	if err != nil {
		if !conn.alive() {
			a.endJob(job, lostConnectionMessage(err))
		} else {
			a.endJob(job, "The installation on the Frame did not finish (see the messages above). Installing again is always safe.")
		}
		return
	}
	a.endJob(job, "")
}

// uninstall removes CitraVR's library entry, its artwork and its files in the title folder; its data (folders, games and
// saves) stays on the Frame
func (a *app) uninstall() {
	job, err := a.beginJob("uninstall")
	if err != nil {
		return
	}
	conn, err := a.connection()
	if err != nil {
		a.endJob(job, lostConnectionMessage(err))
		return
	}
	a.addLine(job, "==> Removing CitraVR from the Steam library", false)
	err = a.runInstaller(conn, job, "https://github.com/"+repo+"/releases/latest/download/install.sh", "--uninstall")
	if err != nil {
		a.endJob(job, "CitraVR could not be removed (see the messages above): "+err.Error())
		return
	}
	a.endJob(job, "")
}

// trustNewKey forgets the Frame's old host key, then signs in again
func (a *app) trustNewKey(password string) {
	a.mu.Lock()
	address := a.state.Address
	a.mu.Unlock()
	_, host := splitUser(address)
	updateSettings(func(s *settings) { delete(s.HostKeys, strings.ToLower(host)) })
	a.runChecks(address, password)
}

// The page's API

type requestBody struct {
	Address  string `json:"address"`
	Password string `json:"password"`
}

func (a *app) routes(mux *http.ServeMux) {
	handle := func(path string, method string, f func(w http.ResponseWriter, r *http.Request)) {
		mux.HandleFunc(path, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != method || r.Header.Get("X-Token") != a.token {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
			f(w, r)
		})
	}
	decode := func(r *http.Request) (requestBody, error) {
		var b requestBody
		err := json.NewDecoder(io.LimitReader(r.Body, 64*1024)).Decode(&b)
		return b, err
	}
	handle("/api/state", "GET", func(w http.ResponseWriter, r *http.Request) {
		a.lastPoll.Store(time.Now().UnixNano())
		a.mu.Lock()
		data, _ := json.Marshal(a.state)
		a.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(data)
	})
	handle("/api/connect", "POST", func(w http.ResponseWriter, r *http.Request) {
		b, err := decode(r)
		if err != nil {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		go a.runChecks(b.Address, b.Password)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/trust-key", "POST", func(w http.ResponseWriter, r *http.Request) {
		b, _ := decode(r)
		go a.trustNewKey(b.Password)
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/discover", "POST", func(w http.ResponseWriter, r *http.Request) {
		go func() {
			a.update(func(s *appState) { s.Discovering = true })
			frames := discoverFrames(2 * time.Second)
			a.update(func(s *appState) { s.Discovering = false; s.Frames = frames })
		}()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/latest", "POST", func(w http.ResponseWriter, r *http.Request) {
		go a.fetchRelease()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/install", "POST", func(w http.ResponseWriter, r *http.Request) {
		go a.install()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/uninstall", "POST", func(w http.ResponseWriter, r *http.Request) {
		go a.uninstall()
		w.WriteHeader(http.StatusNoContent)
	})
	handle("/api/quit", "POST", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
		a.quitOnce.Do(func() { close(a.quit) })
	})
}

// waitForPageClosed (browser only): returns when Quit is pressed, or when the page stopped asking for the state (closed tab)
// while no job runs
func (a *app) waitForPageClosed(ctx context.Context) {
	a.update(func(s *appState) { s.Browser = true })
	opened := time.Now()
	for {
		select {
		case <-a.quit:
			return
		case <-ctx.Done():
			return
		case <-time.After(2 * time.Second):
		}
		a.mu.Lock()
		busy := a.busy
		a.mu.Unlock()
		last := a.lastPoll.Load()
		if busy {
			continue
		}
		if last == 0 && time.Since(opened) > 30*time.Minute {
			return
		}
		if last != 0 && time.Since(time.Unix(0, last)) > 20*time.Second {
			return
		}
	}
}

func (a *app) shutdown() {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn != nil {
		a.conn.close()
	}
}
