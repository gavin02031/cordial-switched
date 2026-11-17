import unittest

from export_horizon_providers import export_from_report


BASE_REPORT = """# Morrow native engine entry readiness attempt
morrow_build_version=0.99.4
event=full_final_base_audit_started
full_audit_status=4
full_audit_relocation_scan_complete=yes
full_audit_import_lookups=4
full_audit_supported_imports=3
full_audit_missing_imports=1
full_audit_missing_imports_reported=1
full_audit_missing_import_list_complete=yes
full_audit_missing_import_001=futex @ LIBC from libc.so
full_audit_resolved_imports_reported=3
full_audit_resolved_import_list_complete=yes
full_audit_resolved_import_001=pread @ LIBC from libc.so
full_audit_resolved_import_002=pread @ LIBC from libc.so
full_audit_resolved_import_003=close @ unversioned from unspecified
"""


class HorizonProviderExportTests(unittest.TestCase):
    def test_completed_target_resolver_observation_is_deduplicated(self):
        providers, evidence = export_from_report(BASE_REPORT)
        self.assertEqual(providers, [
            {'name': 'close', 'version': None, 'library': None},
            {'name': 'pread', 'version': 'LIBC', 'library': 'libc.so'},
        ])
        self.assertEqual(evidence['resolved_import_entries'], 3)
        self.assertEqual(evidence['distinct_resolved_abi_keys'], 2)
        self.assertEqual(evidence['missing_import_entries'], 1)
        self.assertEqual(evidence['morrow_build_version'], '0.99.4')

    def test_does_not_accept_partial_relocation_scans(self):
        for report in [
            BASE_REPORT.replace('relocation_scan_complete=yes',
                                'relocation_scan_complete=no'),
            BASE_REPORT.replace('full_audit_status=4', 'full_audit_status=0'),
            BASE_REPORT.replace('full_audit_status=4', 'full_audit_status=2'),
            BASE_REPORT.replace('event=full_final_base_audit_started',
                                'event=stage_failed_before_audit'),
        ]:
            with self.subTest(report=report[-80:]), self.assertRaises(ValueError):
                export_from_report(report)

    def test_truncated_or_inconsistent_records_fail_closed(self):
        for report in [
            BASE_REPORT.replace('resolved_import_list_complete=yes',
                                'resolved_import_list_complete=no'),
            BASE_REPORT.replace('missing_import_list_complete=yes',
                                'missing_import_list_complete=no'),
            BASE_REPORT.replace('full_audit_import_lookups=4',
                                'full_audit_import_lookups=7'),
            BASE_REPORT.replace('full_audit_resolved_import_002=', 'full_audit_resolved_import_005='),
            BASE_REPORT.replace('full_audit_missing_import_001=', 'full_audit_missing_import_002='),
            BASE_REPORT.replace('full_audit_resolved_import_003=',
                                'full_audit_resolved_import_bad='),
            BASE_REPORT.replace('pread @ LIBC from libc.so',
                                'pread @ libc.so', 1),
            BASE_REPORT + 'full_audit_resolved_import_001=pwrite @ LIBC from libc.so\n',
            BASE_REPORT.replace('pread @ LIBC from libc.so', '? @ LIBC from libc.so', 1),
        ]:
            with self.subTest(report=report[-100:]), self.assertRaises(ValueError):
                export_from_report(report)

    def test_unknown_version_or_library_does_not_become_provider_evidence(self):
        # The target logger uses '?' for unavailable ABI metadata. Such
        # placeholders must not silently become literal version/DSO keys.
        for record in ('pread @ ? from libc.so',
                       'pread @ LIBC from ?'):
            report = BASE_REPORT.replace(
                'resolved_import_001=pread @ LIBC from libc.so',
                'resolved_import_001=' + record)
            with self.subTest(record=record), self.assertRaisesRegex(
                    ValueError, 'incomplete ABI key'):
                export_from_report(report)
        # The explicit unversioned/unspecified sentinel pair is legitimate.
        providers, _ = export_from_report(BASE_REPORT)
        self.assertIn({'name': 'close', 'version': None,
                       'library': None}, providers)

    def test_padded_abi_fields_fail_closed(self):
        # Padding changes the exact version/DSO identity and can make an
        # unverified provider appear distinct from the target's import.
        for record in (' pread @ LIBC from libc.so',
                       'pread  @ LIBC from libc.so',
                       'pread @  LIBC from libc.so',
                       'pread @ LIBC from libc.so ',
                       'pread @ LIBC from  '):
            report = BASE_REPORT.replace(
                'resolved_import_001=pread @ LIBC from libc.so',
                'resolved_import_001=' + record)
            with self.subTest(record=record), self.assertRaisesRegex(
                    ValueError, 'incomplete ABI key'):
                export_from_report(report)

    def test_conflicting_observations_fail_closed(self):
        # A resolved and missing record for the same exact ABI key cannot
        # establish a trustworthy provider manifest, even with valid counts.
        contradictory = BASE_REPORT.replace(
            'missing_import_001=futex @ LIBC from libc.so',
            'missing_import_001=pread @ LIBC from libc.so')
        with self.assertRaisesRegex(ValueError, 'both resolved and missing'):
            export_from_report(contradictory)
        # Version/provider identity remains exact: similarly named keys in
        # different ABI namespaces are not contradictory.
        distinct = BASE_REPORT.replace(
            'missing_import_001=futex @ LIBC from libc.so',
            'missing_import_001=pread @ OTHER from libother.so')
        providers, evidence = export_from_report(distinct)
        self.assertEqual(len(providers), 2)
        self.assertEqual(evidence['missing_import_entries'], 1)

    def test_different_abi_providers_are_not_collapsed(self):
        report = BASE_REPORT.replace(
            'resolved_import_002=pread @ LIBC from libc.so',
            'resolved_import_002=pread @ OTHER from libother.so')
        providers, _ = export_from_report(report)
        self.assertEqual(len(providers), 3)
        self.assertIn({'name': 'pread', 'version': 'OTHER',
                       'library': 'libother.so'}, providers)

    def test_complete_audit_with_unsupported_relocation_still_records_keys(self):
        report = BASE_REPORT.replace('full_audit_status=4', 'full_audit_status=3')
        providers, evidence = export_from_report(report)
        self.assertEqual(len(providers), 2)
        self.assertTrue(evidence['relocation_scan_complete'])


if __name__ == '__main__':
    unittest.main()
