#pragma once

void startBuzzer();
void dismiss();
void handleBuzzerEscalation(); // call every loop iteration; non-blocking

// Clear todayCancelled only if today's alarm is still in the future.
// Prevents re-firing when the user saves a schedule after today's alarm already passed.
void resetTodayCancelledIfSafe();
