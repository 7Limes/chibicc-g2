// expect-error: integer constant expression required
void tick() { switch (1) { case 1.0: break; } }
