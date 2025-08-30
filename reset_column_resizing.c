/* RESET COLUMN RESIZING: Reset all height and width factors when a new tile is added to a column */

#include "nixtile.h"

/* Reset all resizing factors (height and width) for a specific column */
static void
reset_column_resizing_factors(Monitor *m, int target_column)
{
    if (!m || target_column < 0) {
        wlr_log(WLR_ERROR, "[nixtile] RESET RESIZING: Invalid parameters - monitor=%p, column=%d", 
                (void*)m, target_column);
        return;
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** RESET COLUMN RESIZING *** Column %d - RESETTING ALL FACTORS", target_column);
    
    /* Count tiles in the target column */
    int tiles_in_column = 0;
    Client *c;
    wl_list_for_each(c, &clients, link) {
        if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
            c->column_group == target_column) {
            tiles_in_column++;
        }
    }
    
    if (tiles_in_column <= 1) {
        wlr_log(WLR_DEBUG, "[nixtile] RESET RESIZING: Column %d has %d tiles - no reset needed", 
                target_column, tiles_in_column);
        return;
    }
    
    wlr_log(WLR_ERROR, "[nixtile] RESET RESIZING: Column %d has %d tiles - resetting all factors to 1.0", 
            target_column, tiles_in_column);
    
    /* Reset both height and width factors for all tiles in the column */
    int reset_count = 0;
    wl_list_for_each(c, &clients, link) {
        if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
            c->column_group == target_column) {
            
            float old_height_factor = c->height_factor;
            float old_width_factor = c->width_factor;
            
            /* Reset to equal distribution */
            c->height_factor = 1.0f;
            c->width_factor = 1.0f;
            
            wlr_log(WLR_ERROR, "[nixtile] RESET RESIZING: Tile %p column %d: height %.3f->1.000, width %.3f->1.000", 
                    (void*)c, target_column, old_height_factor, old_width_factor);
            
            reset_count++;
        }
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** RESET COMPLETE *** Column %d: Reset %d tiles to equal distribution", 
            target_column, reset_count);
}

/* Reset all resizing factors for all columns that have multiple tiles */
void
reset_all_column_resizing_factors(Monitor *m)
{
    if (!m) {
        wlr_log(WLR_ERROR, "[nixtile] RESET ALL RESIZING: NULL monitor");
        return;
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** RESET ALL COLUMN RESIZING *** Starting full resizing reset");
    
    int optimal_columns = get_workspace_optimal_columns();
    
    /* Reset each column that has multiple tiles */
    for (int col = 0; col < optimal_columns && col < MAX_COLUMNS; col++) {
        int tiles_in_column = count_tiles_in_stack(col, m);
        if (tiles_in_column > 1) {
            wlr_log(WLR_ERROR, "[nixtile] RESET ALL RESIZING: Column %d has %d tiles - resetting", 
                    col, tiles_in_column);
            reset_column_resizing_factors(m, col);
        }
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** RESET ALL COMPLETE *** Full resizing reset complete");
}

/* Reset resizing factors when a tile is moved to a new column */
void
reset_resizing_on_tile_movement(Monitor *m, int target_column, Client *moved_tile)
{
    if (!m || target_column < 0 || !moved_tile) {
        wlr_log(WLR_ERROR, "[nixtile] RESET ON MOVEMENT: Invalid parameters");
        return;
    }
    
    wlr_log(WLR_ERROR, "[nixtile] *** RESET ON TILE MOVEMENT *** Tile %p moved to column %d", 
            (void*)moved_tile, target_column);
    
    /* Reset the target column since it now has a new tile */
    reset_column_resizing_factors(m, target_column);
    
    /* Save the reset state to workspace */
    save_workspace_state();
    
    wlr_log(WLR_ERROR, "[nixtile] RESET ON MOVEMENT: Saved reset state to workspace");
}
