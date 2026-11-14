// Scene Multiview Dock for OBS Studio 31+/33 (Qt6) - Windows build
// A single dock that shows 2/4/6/8 live scene previews, lets you choose which
// scene goes in each cell, shows tally borders (red = Program, green = Preview),
// click to switch scene, and can be opened in a separate/fullscreen window.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>

#include <QAction>
#include <QApplication>
#include <QObject>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Scene Multiview Dock";
}
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Dock with 2/4/6/8 selectable live scene previews";
}

static constexpr int kMaxCells = 16; // 2 columns x up to 8 rows
static const char *kDockId = "scene_multiview";

// ---------------------------------------------------------------- model ----
struct Model {
	int rows = 4;                    // scene rows (2 columns each): 1..8
	std::vector<std::string> scenes; // scene name per cell (size kMaxCells)
	std::string previewScene;        // plugin-side Preview bus (used when Studio Mode is off)
	bool edit = true;                // show scene pickers under each cell
	Model() : scenes(kMaxCells) {}
};
static Model g_model;

class MultiviewGrid;
static std::vector<MultiviewGrid *> g_grids;

static void refreshAll();
static void rebuildAll();

static std::vector<std::string> sceneNames()
{
	std::vector<std::string> v;
	obs_frontend_source_list l = {};
	obs_frontend_get_scenes(&l);
	for (size_t i = 0; i < l.sources.num; i++)
		v.emplace_back(obs_source_get_name(l.sources.array[i]));
	obs_frontend_source_list_free(&l);
	return v;
}

static std::string currentName(bool preview)
{
	obs_source_t *s = preview ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
	std::string n;
	if (s) {
		n = obs_source_get_name(s);
		obs_source_release(s);
	}
	return n;
}

static void fillDefaultsIfEmpty()
{
	for (auto &s : g_model.scenes)
		if (!s.empty())
			return;
	auto names = sceneNames();
	for (size_t i = 0; i < names.size() && i < (size_t)kMaxCells; i++)
		g_model.scenes[i] = names[i];
}

// -------------------------------------------------------- display widget ----
class DisplayWidget : public QWidget {
	Q_OBJECT
public:
	explicit DisplayWidget(bool autoAspect, QWidget *parent = nullptr) : QWidget(parent), aspect(autoAspect)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_NativeWindow);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NoSystemBackground);
		setMinimumSize(96, 54);
		QSizePolicy sp(QSizePolicy::Expanding, aspect ? QSizePolicy::Preferred : QSizePolicy::Expanding);
		sp.setHeightForWidth(aspect);
		setSizePolicy(sp);
	}

	bool hasHeightForWidth() const override { return aspect; }
	int heightForWidth(int w) const override
	{
		double r = 9.0 / 16.0;
		obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.base_width)
			r = (double)ovi.base_height / (double)ovi.base_width;
		return (int)(w * r);
	}
	~DisplayWidget() override
	{
		destroyDisplay();
		releaseSource();
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	void setScene(const std::string &name)
	{
		bool expired = false;
		{
			std::lock_guard<std::mutex> l(mtx);
			expired = weak && obs_weak_source_expired(weak);
		}
		if (name == sceneName && !expired)
			return;
		releaseSource();
		sceneName = name;
		if (name.empty())
			return;
		obs_source_t *s = obs_get_source_by_name(name.c_str());
		if (!s)
			return;
		obs_source_inc_showing(s);
		obs_weak_source_t *w = obs_source_get_weak_source(s);
		obs_source_release(s);
		std::lock_guard<std::mutex> l(mtx);
		weak = w;
	}

signals:
	void clicked();
	void doubleClicked();
	void contextRequested(const QPoint &globalPos);

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		createDisplay();
	}
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		createDisplay();
		if (display) {
			QSize s = size() * devicePixelRatioF();
			obs_display_resize(display, s.width(), s.height());
		}
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			emit clicked();
		else if (e->button() == Qt::RightButton)
			emit contextRequested(e->globalPosition().toPoint());
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			emit doubleClicked();
	}

private:
	bool aspect;
	obs_display_t *display = nullptr;
	obs_weak_source_t *weak = nullptr;
	std::string sceneName;
	std::mutex mtx;

	void releaseSource()
	{
		obs_weak_source_t *old;
		{
			std::lock_guard<std::mutex> l(mtx);
			old = weak;
			weak = nullptr;
		}
		if (old) {
			obs_source_t *o = obs_weak_source_get_source(old);
			if (o) {
				obs_source_dec_showing(o);
				obs_source_release(o);
			}
			obs_weak_source_release(old);
		}
		sceneName.clear();
	}

	void createDisplay()
	{
		if (display || !isVisible() || !windowHandle())
			return;
#ifdef _WIN32
		QSize s = size() * devicePixelRatioF();
		if (s.width() < 2 || s.height() < 2)
			return;
		gs_init_data info = {};
		info.cx = (uint32_t)s.width();
		info.cy = (uint32_t)s.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		info.window.hwnd = reinterpret_cast<void *>(winId());
		display = obs_display_create(&info, 0xFF000000);
		if (display)
			obs_display_add_draw_callback(display, drawCb, this);
#endif
	}

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, drawCb, this);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	static void drawCb(void *data, uint32_t cx, uint32_t cy)
	{
		auto *w = static_cast<DisplayWidget *>(data);
		obs_source_t *src = nullptr;
		{
			std::lock_guard<std::mutex> l(w->mtx);
			if (w->weak)
				src = obs_weak_source_get_source(w->weak);
		}
		if (!src)
			return;

		uint32_t sw = obs_source_get_base_width(src);
		uint32_t sh = obs_source_get_base_height(src);
		if (!sw || !sh) {
			obs_video_info ovi;
			if (obs_get_video_info(&ovi)) {
				sw = ovi.base_width;
				sh = ovi.base_height;
			} else {
				sw = 1920;
				sh = 1080;
			}
		}
		float scale = std::min(cx / (float)sw, cy / (float)sh);
		int nw = (int)(sw * scale), nh = (int)(sh * scale);
		int x = ((int)cx - nw) / 2, y = ((int)cy - nh) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_set_viewport(x, y, nw, nh);
		obs_source_video_render(src);
		gs_projection_pop();
		gs_viewport_pop();

		obs_source_release(src);
	}
};

// ------------------------------------------------------------------ cell ----
class Cell : public QFrame {
	Q_OBJECT
public:
	Cell(int idx, bool aspect, QWidget *parent) : QFrame(parent), index(idx)
	{
		setObjectName("svcell");
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(3, 3, 3, 3);
		l->setSpacing(2);
		disp = new DisplayWidget(aspect, this);
		combo = new QComboBox(this);
		label = new QLabel(this);
		label->setAlignment(Qt::AlignCenter);
		l->addWidget(disp, 1);
		l->addWidget(combo);
		l->addWidget(label);

		connect(combo, &QComboBox::activated, this,
			[this](int i) { emit sceneChosen(index, combo->itemData(i).toString().toStdString()); });
		connect(disp, &DisplayWidget::clicked, this, [this] { emit cellClicked(index, false); });
		connect(disp, &DisplayWidget::doubleClicked, this, [this] { emit cellClicked(index, true); });
		connect(disp, &DisplayWidget::contextRequested, this, &Cell::contextRequested);
		setTally(0);
	}

	void update(const std::vector<std::string> &names, const std::string &assigned, int tally, bool showPicker)
	{
		{
			QSignalBlocker b(combo);
			combo->clear();
			combo->addItem(QStringLiteral("(none)"), QString());
			for (auto &n : names)
				combo->addItem(QString::fromStdString(n), QString::fromStdString(n));
			int i = combo->findData(QString::fromStdString(assigned));
			if (i < 0 && !assigned.empty()) {
				combo->insertItem(1, QString::fromStdString("\u26A0 " + assigned),
						  QString::fromStdString(assigned));
				i = 1;
			}
			combo->setCurrentIndex(i < 0 ? 0 : i);
		}
		label->setText(assigned.empty() ? QStringLiteral("-") : QString::fromStdString(assigned));
		combo->setVisible(showPicker);
		label->setVisible(!showPicker);
		disp->setScene(assigned);
		setTally(tally);
	}

signals:
	void sceneChosen(int index, const std::string &name);
	void cellClicked(int index, bool dbl);
	void contextRequested(const QPoint &globalPos);

private:
	int index;
	DisplayWidget *disp;
	QComboBox *combo;
	QLabel *label;

	void setTally(int t)
	{
		const char *col = t == 2 ? "#e53935" : t == 1 ? "#43a047" : "#6b7686";
		setStyleSheet(QString("QFrame#svcell{border:3px solid %1;background:#000;}").arg(col));
	}
};

// -------------------------------------------------------------- bus cell ----
class BusCell : public QFrame {
	Q_OBJECT
public:
	BusCell(const QString &t, const QString &color, bool aspect, QWidget *parent) : QFrame(parent), title(t)
	{
		setObjectName("bus");
		setStyleSheet(QString("QFrame#bus{border:4px solid %1;background:#000;}").arg(color));
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(3, 3, 3, 3);
		l->setSpacing(2);
		disp = new DisplayWidget(aspect, this);
		label = new QLabel(this);
		label->setAlignment(Qt::AlignCenter);
		label->setStyleSheet(QString("color:%1;font-weight:bold;").arg(color));
		l->addWidget(disp, 1);
		l->addWidget(label);
		connect(disp, &DisplayWidget::contextRequested, this, &BusCell::contextRequested);
	}

	void update(const std::string &scene)
	{
		disp->setScene(scene);
		label->setText(scene.empty() ? title : title + QString::fromStdString("  \u00B7  " + scene));
	}

signals:
	void contextRequested(const QPoint &globalPos);

private:
	QString title;
	DisplayWidget *disp;
	QLabel *label;
};

// Cut a scene to Program (plugin mode, Studio Mode off): the scene that was on air goes to Preview.
static void cutToProgram(const std::string &name)
{
	if (name.empty())
		return;
	const std::string old = currentName(false);
	obs_source_t *s = obs_get_source_by_name(name.c_str());
	if (!s)
		return;
	obs_frontend_set_current_scene(s);
	obs_source_release(s);
	if (!obs_frontend_preview_program_mode_active() && !old.empty() && old != name)
		g_model.previewScene = old;
}

// TAKE button. In Studio Mode this uses OBS' own transition button.
static void takeToProgram()
{
	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_preview_program_trigger_transition();
		return;
	}
	cutToProgram(g_model.previewScene);
	refreshAll();
}

// ------------------------------------------------------------------ grid ----
class MultiviewGrid;
static void openProjector(QScreen *fullscreenOn);

class MultiviewGrid : public QWidget {
	Q_OBJECT
public:
	// pickers = true: dock (Preview/Program + TAKE pinned on top, scenes scroll, 2 columns, 16:9 cells)
	// pickers = false: projector window (fills the window, 4 scene columns)
	explicit MultiviewGrid(bool pickers, QWidget *parent = nullptr) : QWidget(parent), allowPickers(pickers)
	{
		root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(4);
		g_grids.push_back(this);
		rebuild();
	}
	~MultiviewGrid() override { g_grids.erase(std::remove(g_grids.begin(), g_grids.end(), this), g_grids.end()); }

	void rebuild()
	{
		cells.clear();
		pvw = pgm = nullptr;
		takeBtn = nullptr;
		delete topW;
		topW = nullptr;
		if (scroll) {
			delete scroll;
			scroll = nullptr;
			scenesW = nullptr;
		} else {
			delete scenesW;
			scenesW = nullptr;
		}

		// --- top: Preview | Program (+ TAKE) ---
		topW = new QWidget(this);
		auto *tg = new QGridLayout(topW);
		tg->setContentsMargins(0, 0, 0, 0);
		tg->setSpacing(4);
		pvw = new BusCell("PREVIEW", "#43a047", allowPickers, topW);
		pgm = new BusCell("PROGRAM", "#e53935", allowPickers, topW);
		connect(pvw, &BusCell::contextRequested, this, &MultiviewGrid::showMenu);
		connect(pgm, &BusCell::contextRequested, this, &MultiviewGrid::showMenu);
		tg->addWidget(pvw, 0, 0);
		tg->addWidget(pgm, 0, 1);
		tg->setColumnStretch(0, 1);
		tg->setColumnStretch(1, 1);
		if (allowPickers) {
			takeBtn = new QPushButton("TAKE  ▶  Preview → Program", topW);
			takeBtn->setMinimumHeight(34);
			takeBtn->setStyleSheet("font-weight:bold;");
			connect(takeBtn, &QPushButton::clicked, this, [] { takeToProgram(); });
			tg->addWidget(takeBtn, 1, 0, 1, 2);
		} else {
			tg->setRowStretch(0, 1);
		}

		// --- scenes ---
		scenesW = new QWidget;
		auto *sg = new QGridLayout(scenesW);
		sg->setContentsMargins(0, 0, 0, 0);
		sg->setSpacing(4);
		const int n = g_model.rows * 2;
		const int cols = allowPickers ? 2 : std::min(4, n);
		const int rowsN = (n + cols - 1) / cols;
		for (int i = 0; i < n; i++) {
			auto *c = new Cell(i, allowPickers, scenesW);
			connect(c, &Cell::sceneChosen, this, [](int idx, const std::string &name) {
				g_model.scenes[idx] = name;
				refreshAll();
			});
			connect(c, &Cell::cellClicked, this, &MultiviewGrid::onClick);
			connect(c, &Cell::contextRequested, this, &MultiviewGrid::showMenu);
			sg->addWidget(c, i / cols, i % cols);
			cells.push_back(c);
		}
		for (int k = 0; k < cols; k++)
			sg->setColumnStretch(k, 1);
		if (allowPickers) {
			sg->setRowStretch(rowsN, 1); // spare height stays empty at the bottom
		} else {
			for (int r = 0; r < rowsN; r++)
				sg->setRowStretch(r, 1);
		}

		if (allowPickers) {
			scroll = new QScrollArea(this);
			scroll->setWidgetResizable(true);
			scroll->setFrameShape(QFrame::NoFrame);
			scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scroll->setWidget(scenesW);
			root->addWidget(topW, 0);
			root->addWidget(scroll, 1);
		} else {
			scenesW->setParent(this);
			root->addWidget(topW, 3);
			root->addWidget(scenesW, rowsN);
		}
		refresh();
	}

	void refresh()
	{
		auto names = sceneNames();
		std::string prog = currentName(false);
		std::string prev = obs_frontend_preview_program_mode_active() ? currentName(true)
									      : g_model.previewScene;
		if (pvw)
			pvw->update(prev);
		if (pgm)
			pgm->update(prog);
		for (size_t i = 0; i < cells.size(); i++) {
			const std::string &a = g_model.scenes[i];
			// Only offer scenes that are not already used in another cell.
			std::vector<std::string> avail;
			for (auto &nm : names) {
				bool usedElsewhere = false;
				if (nm != a)
					for (size_t j = 0; j < cells.size(); j++)
						if (j != i && g_model.scenes[j] == nm) {
							usedElsewhere = true;
							break;
						}
				if (!usedElsewhere)
					avail.push_back(nm);
			}
			int tally = (!a.empty() && a == prog) ? 2 : (!a.empty() && a == prev) ? 1 : 0;
			cells[i]->update(avail, a, tally, allowPickers && g_model.edit);
		}
	}

private:
	bool allowPickers;
	QVBoxLayout *root;
	QWidget *topW = nullptr;
	QWidget *scenesW = nullptr;
	QScrollArea *scroll = nullptr;
	std::vector<Cell *> cells;
	BusCell *pvw = nullptr;
	BusCell *pgm = nullptr;
	QPushButton *takeBtn = nullptr;

	// Click = send to Preview bus (OBS preview if Studio Mode is on).
	// Double-click = straight to Program.
	void onClick(int i, bool dbl)
	{
		const std::string name = g_model.scenes[i];
		if (name.empty())
			return;
		bool studio = obs_frontend_preview_program_mode_active();
		if (!dbl && !studio) {
			g_model.previewScene = name;
			refreshAll();
			return;
		}
		if (dbl && !studio) {
			cutToProgram(name);
			refreshAll();
			return;
		}
		obs_source_t *s = obs_get_source_by_name(name.c_str());
		if (!s)
			return;
		if (dbl)
			obs_frontend_set_current_scene(s);
		else
			obs_frontend_set_current_preview_scene(s);
		obs_source_release(s);
	}

	void showMenu(const QPoint &gp)
	{
		QMenu m;
		connect(m.addAction("Open in window"), &QAction::triggered, [] { openProjector(nullptr); });
		QMenu *fs = m.addMenu("Fullscreen on screen");
		for (QScreen *s : QGuiApplication::screens()) {
			QString t = QString("%1  (%2x%3)")
					    .arg(s->name())
					    .arg(s->geometry().width())
					    .arg(s->geometry().height());
			connect(fs->addAction(t), &QAction::triggered, [s] { openProjector(s); });
		}
		m.exec(gp);
	}
};

// ------------------------------------------------------------- projector ----
class ProjectorWindow : public QWidget {
	Q_OBJECT
public:
	ProjectorWindow() : QWidget(nullptr, Qt::Window)
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle("Scene Multiview");
		setStyleSheet("background:#000;");
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(4, 4, 4, 4);
		l->addWidget(new MultiviewGrid(false, this));
	}

protected:
	void keyPressEvent(QKeyEvent *e) override
	{
		if (e->key() == Qt::Key_Escape)
			close();
		else if (e->key() == Qt::Key_F11)
			isFullScreen() ? showNormal() : showFullScreen();
		else
			QWidget::keyPressEvent(e);
	}
};

static void openProjector(QScreen *fullscreenOn)
{
	auto *w = new ProjectorWindow();
	if (fullscreenOn) {
		w->move(fullscreenOn->geometry().topLeft());
		w->showFullScreen();
	} else {
		w->resize(1280, 720);
		w->show();
	}
}

// ------------------------------------------------------------------ dock ----
class MultiviewDock : public QWidget {
	Q_OBJECT
public:
	MultiviewDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(4, 4, 4, 4);
		auto *h = new QHBoxLayout;
		h->addWidget(new QLabel("Rows"));
		countBox = new QComboBox;
		for (int n = 1; n <= 8; n++)
			countBox->addItem(QString("%1 row%2 (%3 scenes)").arg(n).arg(n > 1 ? "s" : "").arg(n * 2), n);
		h->addWidget(countBox);
		editBox = new QCheckBox("Choose scenes");
		h->addWidget(editBox);
		auto *win = new QPushButton("Window");
		h->addWidget(win);
		h->addStretch(1);
		v->addLayout(h);
		grid = new MultiviewGrid(true, this);
		v->addWidget(grid, 1);

		connect(countBox, &QComboBox::activated, this, [this](int i) {
			g_model.rows = countBox->itemData(i).toInt();
			rebuildAll();
		});
		connect(editBox, &QCheckBox::toggled, this, [](bool on) {
			g_model.edit = on;
			refreshAll();
		});
		connect(win, &QPushButton::clicked, this, [] { openProjector(nullptr); });
		syncFromModel();
	}

	void syncFromModel()
	{
		QSignalBlocker a(countBox), b(editBox);
		countBox->setCurrentIndex(std::max(0, countBox->findData(g_model.rows)));
		editBox->setChecked(g_model.edit);
	}

private:
	QComboBox *countBox;
	QCheckBox *editBox;
	MultiviewGrid *grid;
};

static MultiviewDock *g_dock = nullptr;
static bool g_exiting = false;

static void refreshAll()
{
	for (auto *g : std::vector<MultiviewGrid *>(g_grids))
		g->refresh();
}
static void rebuildAll()
{
	for (auto *g : std::vector<MultiviewGrid *>(g_grids))
		g->rebuild();
}

// ------------------------------------------------- save / load / events ----
static void saveCb(obs_data_t *save_data, bool saving, void *)
{
	if (saving) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "rows", g_model.rows);
		obs_data_set_string(d, "preview_scene", g_model.previewScene.c_str());
		obs_data_set_bool(d, "edit", g_model.edit);
		obs_data_array_t *arr = obs_data_array_create();
		for (auto &s : g_model.scenes) {
			obs_data_t *it = obs_data_create();
			obs_data_set_string(it, "scene", s.c_str());
			obs_data_array_push_back(arr, it);
			obs_data_release(it);
		}
		obs_data_set_array(d, "cells", arr);
		obs_data_array_release(arr);
		obs_data_set_obj(save_data, "scene_multiview", d);
		obs_data_release(d);
		return;
	}

	g_model = Model();
	obs_data_t *d = obs_data_get_obj(save_data, "scene_multiview");
	if (d) {
		int r = (int)obs_data_get_int(d, "rows");
		if (r >= 1 && r <= 8)
			g_model.rows = r;
		g_model.previewScene = obs_data_get_string(d, "preview_scene");
		if (obs_data_has_user_value(d, "edit"))
			g_model.edit = obs_data_get_bool(d, "edit");
		obs_data_array_t *arr = obs_data_get_array(d, "cells");
		if (arr) {
			size_t n = std::min((size_t)kMaxCells, obs_data_array_count(arr));
			for (size_t i = 0; i < n; i++) {
				obs_data_t *it = obs_data_array_item(arr, i);
				g_model.scenes[i] = obs_data_get_string(it, "scene");
				obs_data_release(it);
			}
			obs_data_array_release(arr);
		}
		obs_data_release(d);
	}
	if (g_dock)
		g_dock->syncFromModel();
	rebuildAll();
}

// Keep scene names in the model in sync when a scene is renamed in OBS.
static void renameCb(void *, calldata_t *cd)
{
	const char *n = calldata_string(cd, "new_name");
	const char *p = calldata_string(cd, "prev_name");
	if (!n || !p || g_exiting || !g_dock)
		return;
	std::string newName = n, prevName = p;
	QMetaObject::invokeMethod(
		g_dock,
		[newName, prevName] {
			for (auto &s : g_model.scenes)
				if (s == prevName)
					s = newName;
			if (g_model.previewScene == prevName)
				g_model.previewScene = newName;
			refreshAll();
		},
		Qt::QueuedConnection);
}

static void eventCb(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		g_exiting = true;
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", renameCb, nullptr);
		for (QWidget *w : QApplication::topLevelWidgets())
			if (dynamic_cast<ProjectorWindow *>(w))
				w->close();
		obs_frontend_remove_dock(kDockId);
		g_dock = nullptr;
		return;
	}
	if (g_exiting || !g_dock)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		QMetaObject::invokeMethod(
			g_dock,
			[event] {
				if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
					fillDefaultsIfEmpty();
				refreshAll();
			},
			Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	g_dock = new MultiviewDock();
	obs_frontend_add_dock_by_id(kDockId, "Scene Multiview", g_dock);
	obs_frontend_add_save_callback(saveCb, nullptr);
	obs_frontend_add_event_callback(eventCb, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", renameCb, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(saveCb, nullptr);
	obs_frontend_remove_event_callback(eventCb, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", renameCb, nullptr);
}

#include "scene-multiview.moc"
