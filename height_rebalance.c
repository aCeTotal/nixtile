/* AGGRESSIVE EQUAL HEIGHT DISTRIBUTION: Nuclear option to force equal heights */

static void
rebalance_column_heights(Monitor *m, int target_column)
{
	wlr_log(WLR_ERROR, "[nixtile] *** NUCLEAR REBALANCE START *** Column %d - FORCING EQUAL DISTRIBUTION", target_column);
	
	if (!m || target_column < 0) {
		wlr_log(WLR_ERROR, "[nixtile] HEIGHT REBALANCE: Invalid parameters - monitor=%p, column=%d", 
		        (void*)m, target_column);
		return;
	}
	
	/* Count tiles in the target column */
	int tiles_in_column = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
		    c->column_group == target_column) {
			tiles_in_column++;
		}
	}
	
	if (tiles_in_column == 0) {
		wlr_log(WLR_DEBUG, "[nixtile] HEIGHT REBALANCE: No tiles in column %d", target_column);
		return;
	}
	
	/* STEP 1: Force all tiles in column to height_factor = 1.0 (equal distribution) */
	wlr_log(WLR_ERROR, "[nixtile] *** STEP 1 *** Setting all tiles in column %d to height_factor = 1.0", target_column);
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
		    c->column_group == target_column) {
			float old_factor = c->height_factor;
			c->height_factor = 1.0f;
			wlr_log(WLR_ERROR, "[nixtile] TILE RESET: Tile %p column %d: %.3f -> 1.000 (%.1f%%)", 
			        (void*)c, target_column, old_factor, 100.0f / tiles_in_column);
		}
	}
	wlr_log(WLR_ERROR, "[nixtile] *** STEP 1 COMPLETE *** All %d tiles in column %d set to 1.0", tiles_in_column, target_column);
	
	/* STEP 2: Update workspace storage to match the equal distribution */
	int workspace = get_current_workspace();
	if (workspace >= 0 && workspace < 9) {
		/* Update workspace height factors to match the new equal distribution */
		Client *storage_c;
		int storage_index = 0;
		wl_list_for_each(storage_c, &clients, link) {
			if (VISIBLEON(storage_c, m) && storage_index < MAX_TILES_PER_WORKSPACE) {
				/* Set workspace storage to 1.0f for tiles in target column, keep others unchanged */
				if (!storage_c->isfloating && !storage_c->isfullscreen && storage_c->column_group == target_column) {
					m->workspace_height_factors[workspace][storage_index] = 1.0f;
					wlr_log(WLR_ERROR, "[nixtile] WORKSPACE STORAGE: Set storage[%d][%d] = 1.0 for column %d", 
					        workspace, storage_index, target_column);
				}
				storage_index++;
			}
		}
		
		/* Clear manual resize flags for the target column */
		if (m->workspace_manual_resize_performed && target_column < MAX_COLUMNS) {
			m->workspace_manual_resize_performed[workspace][target_column] = false;
		}
		if (target_column < 2) {
			manual_resize_performed[target_column] = false;
		}
		
		wlr_log(WLR_ERROR, "[nixtile] WORKSPACE STORAGE: Updated storage for column %d in workspace %d", target_column, workspace);
	}
	
	/* STEP 3: Force save the cleared state to prevent restoration of old values */
	wlr_log(WLR_ERROR, "[nixtile] *** STEP 3 *** Saving workspace state to preserve equal distribution");
	save_workspace_state();
	wlr_log(WLR_ERROR, "[nixtile] *** STEP 3 COMPLETE *** Workspace state saved");
	
	/* STEP 4: Verify the rebalancing worked */
	wlr_log(WLR_ERROR, "[nixtile] *** VERIFICATION *** Checking final height factors in column %d:", target_column);
	Client *verify_c;
	wl_list_for_each(verify_c, &clients, link) {
		if (VISIBLEON(verify_c, m) && !verify_c->isfloating && !verify_c->isfullscreen && 
		    verify_c->column_group == target_column) {
			wlr_log(WLR_ERROR, "[nixtile] VERIFY: Tile %p height_factor = %.3f (should be 1.000)", 
			        (void*)verify_c, verify_c->height_factor);
		}
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** REBALANCE COMPLETE *** Column %d: %d tiles at %.1f%% each", 
	        target_column, tiles_in_column, 100.0f / tiles_in_column);
}

/* Rebalance heights for all columns that have tiles */
static void
rebalance_all_column_heights(Monitor *m)
{
	if (!m) {
		wlr_log(WLR_ERROR, "[nixtile] HEIGHT REBALANCE ALL: NULL monitor");
		return;
	}
	
	wlr_log(WLR_ERROR, "[nixtile] HEIGHT REBALANCE ALL: Starting full height rebalancing");
	
	int optimal_columns = get_workspace_optimal_columns();
	
	/* Rebalance each column that has tiles */
	for (int col = 0; col < optimal_columns && col < MAX_COLUMNS; col++) {
		int tiles_in_column = count_tiles_in_stack(col, m);
		if (tiles_in_column > 1) {
			wlr_log(WLR_ERROR, "[nixtile] HEIGHT REBALANCE ALL: Column %d has %d tiles - rebalancing", 
			        col, tiles_in_column);
			rebalance_column_heights(m, col);
		}
	}
	
	wlr_log(WLR_ERROR, "[nixtile] HEIGHT REBALANCE ALL: Full rebalancing complete");
}
