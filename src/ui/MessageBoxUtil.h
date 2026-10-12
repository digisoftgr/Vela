#pragma once

class QMessageBox;

// Shared preparation for every confirmation dialog in the application: widen it
// so a full path is readable, and let the user resize it from there.
void prepareMessageBox(QMessageBox &box, int widenFactor = 2);
