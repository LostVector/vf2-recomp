$(BIN)/vf2mesh-profile-tgp: $(call objs,tools/acm/profile-mesh-reuse.cpp $(MACHINE) $(HOST) $(filter-out src/core/mb86233.cpp,$(CORE)) src/core/mb86233_aot.cpp $(GEN_NONE) $(M68000)) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)
