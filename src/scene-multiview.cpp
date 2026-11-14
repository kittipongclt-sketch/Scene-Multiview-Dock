// Scene Multiview Dock for OBS Studio 31+/33 (Qt6) - Windows build
// A single dock that shows 2/4/6/8 live scene previews, lets you choose which
// scene goes in each cell, shows tally borders (red = Program, green = Preview),
// click to switch scene, and can be opened in a separate/fullscreen window.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>

#include <QAction>
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

#ifndef _WIN32
#error "This first version supports Windows only (uses the HWND display)."
#endif

static constexpr int kMaxCells = 8;
static const char *kDockId = "scene_multiview";

// ---------------------------------------------------------------- model ----
struct Model {
	int count = 8;                       // 2, 4, 6 or 8
	std::vector<std::string> scenes;     // scene name per cell (size kMaxCells)
	int clickMode = 0;                   // 0 = Auto (Preview in Studio Mode), 1 = always Program
	bool edit = true;                    // show scene pickers under each cell
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
	explicit DisplayWidget(QWidget *parent = nullptr) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_NativeWindow);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NoSystemBackground);
		setMinimumSize(96, 54);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
	Cell(int idx, QWidget *parent) : QFrame(parent), index(idx)
	{
		setObjectName("svcell");
		auto *l = new QVBoxLayout(this);
		l->setContentsMargins(3, 3, 3, 3);
		l->setSpacing(2);
		disp = new DisplayWidget(this);
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
		label->setText(assigned.empty() ? QStringLiteral("-")
						: QString::fromStdString(std::to_string(index + 1) + "  \u00B7  " + assigned));
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
		const char *col = t == 2 ? "#e53935" : t == 1 ? "#43a047" : "#2a2f3a";
		setStyleSheet(QString("QFrame#svcell{border:3px solid %1;background:#000;}").arg(col));
	}
};

// ------------------------------------------------------------------ grid ----
class MultiviewGrid;
static void openProjector(QScreen *fullscreenOn);

class MultiviewGrid : public QWidget {
	Q_OBJECT
public:
	explicit MultiviewGrid(bool pickers, QWidget *parent = nullptr) : QWidget(parent), allowPickers(pickers)
	{
		grid = new QGridLayout(this);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(4);
		g_grids.push_back(this);
		rebuild();
	}
	~MultiviewGrid() override { g_grids.erase(std::remove(g_grids.begin(), g_grids.end(), this), g_grids.end()); }

	void rebuild()
	{
		for (auto *c : cells) {
			grid->removeWidget(c);
			delete c;
		}
		cells.clear();
		for (int i = 0; i < kMaxCells; i++) {
			grid->setRowStretch(i, 0);
			grid->setColumnStretch(i, 0);
		}
		const int n = g_model.count;
		const int cols = n <= 2 ? 2 : n <= 4 ? 2 : n <= 6 ? 3 : 4;
		const int rows = (n + cols - 1) / cols;
		for (int i = 0; i < n; i++) {
			auto *c = new Cell(i, this);
			connect(c, &Cell::sceneChosen, this, [](int idx, const std::string &name) {
				g_model.scenes[idx] = name;
				refreshAll();
			});
			connect(c, &Cell::cellClicked, this, &MultiviewGrid::onClick);
			connect(c, &Cell::contextRequested, this, &MultiviewGrid::showMenu);
			grid->addWidget(c, i / cols, i % cols);
			cells.push_back(c);
		}
		for (int r = 0; r < rows; r++)
			grid->setRowStretch(r, 1);
		for (int k = 0; k < cols; k++)
			grid->setColumnStretch(k, 1);
		refresh();
	}

	void refresh()
	{
		auto names = sceneNames();
		std::string prog = currentName(false);
		std::string prev = obs_frontend_preview_program_mode_active() ? currentName(true) : std::string();
		for (size_t i = 0; i < cells.size(); i++) {
			const std::string &a = g_model.scenes[i];
			int tally = (!a.empty() && a == prog) ? 2 : (!a.empty() && a == prev) ? 1 : 0;
			cells[i]->update(names, a, tally, allowPickers && g_model.edit);
		}
	}

private:
	bool allowPickers;
	QGridLayout *grid;
	std::vector<Cell *> cells;

	void onClick(int i, bool dbl)
	{
		const std::string &name = g_model.scenes[i];
		if (name.empty())
			return;
		obs_source_t *s = obs_get_source_by_name(name.c_str());
		if (!s)
			return;
		bool studio = obs_frontend_preview_program_mode_active();
		if (studio && g_model.clickMode == 0 && !dbl)
			obs_frontend_set_current_preview_scene(s);
		else
			obs_frontend_set_current_scene(s);
		obs_source_release(s);
	}

	void showMenu(const QPoint &gp)
	{
		QMenu m;
		connect(m.addAction("Open in window"), &QAction::triggered, [] { openProjector(nullptr); });
		QMenu *fs = m.addMenu("Fullscreen on screen");
		for (QScreen *s : QGuiApplication::screens()) {
			QString t = QString("%1  (%2x%3)").arg(s->name()).arg(s->geometry().width()).arg(s->geometry().height());
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
		h->addWidget(new QLabel("Screens"));
		countBox = new QComboBox;
		for (int n : {2, 4, 6, 8})
			countBox->addItem(QString::number(n), n);
		h->addWidget(countBox);
		editBox = new QCheckBox("Choose scenes");
		h->addWidget(editBox);
		h->addWidget(new QLabel("Click"));
		clickBox = new QComboBox;
		clickBox->addItem("Preview in Studio Mode, else Program", 0);
		clickBox->addItem("Always Program", 1);
		h->addWidget(clickBox);
		auto *win = new QPushButton("Window");
		h->addWidget(win);
		h->addStretch(1);
		v->addLayout(h);
		grid = new MultiviewGrid(true, this);
		v->addWidget(grid, 1);

		connect(countBox, &QComboBox::activated, this, [this](int i) {
			g_model.count = countBox->itemData(i).toInt();
			rebuildAll();
		});
		connect(editBox, &QCheckBox::toggled, this, [](bool on) {
			g_model.edit = on;
			refreshAll();
		});
		connect(clickBox, &QComboBox::activated, this,
			[this](int i) { g_model.clickMode = clickBox->itemData(i).toInt(); });
		connect(win, &QPushButton::clicked, this, [] { openProjector(nullptr); });
		syncFromModel();
	}

	void syncFromModel()
	{
		QSignalBlocker a(countBox), b(editBox), c(clickBox);
		countBox->setCurrentIndex(std::max(0, countBox->findData(g_model.count)));
		editBox->setChecked(g_model.edit);
		clickBox->setCurrentIndex(std::max(0, clickBox->findData(g_model.clickMode)));
	}

private:
	QComboBox *countBox, *clickBox;
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
		obs_data_set_int(d, "count", g_model.count);
		obs_data_set_int(d, "click_mode", g_model.clickMode);
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
		int c = (int)obs_data_get_int(d, "count");
		if (c == 2 || c == 4 || c == 6 || c == 8)
			g_model.count = c;
		g_model.clickMode = (int)obs_data_get_int(d, "click_mode") == 1 ? 1 : 0;
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

static void eventCb(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		g_exiting = true;
		for (QWidget *w : QGuiApplication::topLevelWidgets())
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
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(saveCb, nullptr);
	obs_frontend_remove_event_callback(eventCb, nullptr);
}

#include "scene-multiview.moc"
