#pragma once

// The editor's Survey tab: control points, photo marks, placement in survey
// coordinates. English and Russian; the other languages show the English.

#include "i18n/BeginCatalog.h"

#define SS_GEO_MSG(name, en, ru)                                                  \
    SS_MSG(name, EN(en), JA(en), ZH_HANS(en), ZH_HANT(en), KO(en), DE(en), FR(en), \
           ES(en), PT(en), IT(en), NL(en), RU(ru), TR(en))

namespace spirula {
namespace i18n {
namespace msg {
namespace geo {

SS_GEO_MSG(tab_geo, "Survey", "Геодезия");
SS_GEO_MSG(tab_geo_help,
    "Control points, their marks on the photos, and the model placed in survey coordinates.",
    "Опорные точки, их отметки на фото и привязка модели к системе координат.");
SS_GEO_MSG(no_dataset,
    "No cameras found for this model. Open a reconstruction or a trained run.",
    "Камеры этой модели не найдены. Откройте реконструкцию или результат обучения.");
SS_GEO_MSG(loading_dataset, "Reading cameras...", "Читаю камеры...");
SS_GEO_MSG(dataset_failed, "Cameras could not be read: {0}", "Камеры не прочитались: {0}");
SS_GEO_MSG(sec_catalog, "Control points", "Опорные точки");
SS_GEO_MSG(crs_name, "Coordinate system", "Система координат");
SS_GEO_MSG(crs_hint, "name, for exports", "название, для экспорта");
SS_GEO_MSG(import_catalog, "Import catalog...", "Импорт каталога...");
SS_GEO_MSG(import_catalog_help,
    "Text or CSV, one point per line: name x y h [code]. x is north, y is east, metres.",
    "Текст или CSV, точка на строку: имя x y h [код]. x — север, y — восток, метры.");
SS_GEO_MSG(import_mooncloud, "Import mooncloud project...", "Импорт проекта mooncloud...");
SS_GEO_MSG(import_mooncloud_help,
    "Points and their photo marks from a mooncloud project.json, matched by file name.",
    "Точки и их отметки на фото из project.json mooncloud, по именам файлов.");
SS_GEO_MSG(imported, "Points: {0}; marks: {1}.", "Точек: {0}; отметок: {1}.");
SS_GEO_MSG(import_failed, "Could not read {0}: {1}", "Не удалось прочитать {0}: {1}");
SS_GEO_MSG(path_hint, "path to the file", "путь к файлу");
SS_GEO_MSG(open_file, "Open", "Открыть");
SS_GEO_MSG(save_failed, "Could not save {0}: {1}", "Не удалось сохранить {0}: {1}");
SS_GEO_MSG(col_name, "Point", "Точка");
SS_GEO_MSG(col_x, "X north", "X север");
SS_GEO_MSG(col_y, "Y east", "Y восток");
SS_GEO_MSG(col_h, "H", "H");
SS_GEO_MSG(col_role, "Role", "Роль");
SS_GEO_MSG(col_marks, "Marks", "Отм.");
SS_GEO_MSG(col_dplan, "dPlan", "dПлан");
SS_GEO_MSG(col_dh, "dH", "dH");
SS_GEO_MSG(role_control, "Control", "Опорная");
SS_GEO_MSG(role_check, "Check", "Контрольная");
SS_GEO_MSG(role_off, "Off", "Выкл.");
SS_GEO_MSG(role_help,
    "Control points place the model. Check points only measure how well it fits.",
    "Опорные привязывают модель. Контрольные только проверяют точность.");
SS_GEO_MSG(no_points, "No control points yet. Import a catalog.", "Опорных точек нет. Импортируйте каталог.");
SS_GEO_MSG(mark_photos, "Mark on photos", "Отметить на фото");
SS_GEO_MSG(delete_point, "Delete point", "Удалить точку");
SS_GEO_MSG(sec_place, "Placement", "Привязка");
SS_GEO_MSG(place, "Place by control points", "Привязать по опорным");
SS_GEO_MSG(place_help,
    "Fits move, turn and scale on control points with a height. Needs 3+, each on 2+ photos.",
    "Сдвиг, поворот и масштаб по опорным с высотой. Нужно 3+, каждая на 2+ фото.");
SS_GEO_MSG(op_place, "Place by control points", "Привязка по опорным");
SS_GEO_MSG(place_failed, "Not placed: {0}", "Не привязано: {0}");
SS_GEO_MSG(placed_control, "Control RMS: plan {0} m, height {1} m. Points: {2}.",
    "СКО опорных: план {0} м, высота {1} м. Точек: {2}.");
SS_GEO_MSG(placed_check, "Check RMS: plan {0} m, height {1} m. Points: {2}.",
    "СКО контрольных: план {0} м, высота {1} м. Точек: {2}.");
SS_GEO_MSG(offset_note, "Stored minus offset E {0}, N {1}, H {2}; exports add it back.",
    "Хранится со сдвигом E {0}, N {1}, H {2}; при экспорте он прибавляется.");
SS_GEO_MSG(worst_mark, "Worst mark: {0} px on {1}", "Худшая отметка: {0} px на {1}");
SS_GEO_MSG(export_points, "Export points CSV...", "Экспорт точек CSV...");
SS_GEO_MSG(exported, "Written: {0}", "Записано: {0}");
SS_GEO_MSG(win_marks, "Marks: {0}", "Отметки: {0}");
SS_GEO_MSG(marks_help,
    "Click to mark. Wheel zooms, right drag pans, Del removes the mark here.",
    "Щелчок — отметка. Колесо — масштаб, правая кнопка — сдвиг, Del — снять отметку.");
SS_GEO_MSG(photos, "Photos", "Фото");
SS_GEO_MSG(photo_marked, "marked", "отмечено");
SS_GEO_MSG(photo_predicted, "should be visible", "должна быть видна");
SS_GEO_MSG(photo_failed, "Could not read the photo.", "Фото не читается.");
SS_GEO_MSG(prev_photo, "Previous", "Предыдущее");
SS_GEO_MSG(next_photo, "Next", "Следующее");
SS_GEO_MSG(only_visible, "Only where it should be visible", "Только где должна быть видна");
SS_GEO_MSG(close, "Close", "Закрыть");

}  // namespace geo
}  // namespace msg
}  // namespace i18n
}  // namespace spirula

#undef SS_GEO_MSG
#include "i18n/EndCatalog.h"
