<?php
/*
** Zabbix
** Copyright (C) 2001-2020 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

require_once 'vendor/autoload.php';

require_once dirname(__FILE__).'/../../include/CWebTest.php';
require_once dirname(__FILE__).'/../traits/MacrosTrait.php';

/**
 * Base class for Macros tests.
 */
abstract class testFormMacros extends CWebTest {

	use MacrosTrait;

	const SQL_HOSTS = 'SELECT * FROM hosts ORDER BY hostid';

	public static function getHash() {
		return CDBHelper::getHash(self::SQL_HOSTS);
	}

	/**
	 * Test creating of host or template with Macros.
	 *
	 * @param array	$data			given data provider
	 * @param string $form_type		string used in form selector
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 */
	protected function checkCreate($data, $form_type, $host_type, $is_prototype = false, $lld_id = null) {
		$this->page->login()->open(
			$is_prototype
			? 'host_prototypes.php?form=create&parent_discoveryid='.$lld_id
			: $host_type.'s.php?form=create'
		);

		$form = $this->query('name:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();
		$form->fill([ucfirst($host_type).' name' => $data['Name']]);

		if ($is_prototype) {
			$form->selectTab('Groups');
		}
		$form->fill(['Groups' => 'Zabbix servers']);

		$this->checkMacros($data, $form_type, $data['Name'], $host_type, $is_prototype, $lld_id);
	}

	/**
	 * Test updating Macros in host, host prototype or template.
	 *
	 * @param array	$data			given data provider
	 * @param string $name			name of host where changes are made
	 * @param string $form_type		string used in form selector
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 */
	protected function checkUpdate($data, $name, $form_type, $host_type, $is_prototype = false, $lld_id = null) {
		$id = CDBHelper::getValue('SELECT hostid FROM hosts WHERE host='.zbx_dbstr($name));

		$this->page->login()->open(
			$is_prototype
			? 'host_prototypes.php?form=update&parent_discoveryid='.$lld_id.'&hostid='.$id
			: $host_type.'s.php?form=update&'.$host_type.'id='.$id.'&groupid=0'
		);

		$this->checkMacros($data, $form_type, $name, $host_type, $is_prototype, $lld_id);
	}

	/**
	 * Test removing Macros from host, host prototype or template.
	 *
	 * @param string $name			name of host where changes are made
	 * @param string $form_type		string used in form selector
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 */
	protected function checkRemove($name, $form_type, $host_type, $is_prototype = false, $lld_id = null) {
		$id = CDBHelper::getValue('SELECT hostid FROM hosts WHERE host='.zbx_dbstr($name));

		$this->page->login()->open(
			$is_prototype
			? 'host_prototypes.php?form=update&parent_discoveryid='.$lld_id.'&hostid='.$id
			: $host_type.'s.php?form=update&'.$host_type.'id='.$id.'&groupid=0'
		);

		$form = $this->query('name:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();
		$form->selectTab('Macros');
		$this->removeMacros();
		$form->submit();

		$message = CMessageElement::find()->one();
		$this->assertTrue($message->isGood());

		$this->assertEquals(($is_prototype ? 'Host prototype' : ucfirst($host_type)).' updated', $message->getTitle());

		$this->assertEquals(1, CDBHelper::getCount('SELECT NULL FROM hosts WHERE host='.zbx_dbstr($name)));
		// Check the results in form.
		$this->checkMacrosFields($name, $is_prototype, $lld_id, $host_type, $form_type, null);
	}

	/**
	 * Test changing and resetting global macro on host, prototype or template.
	 *
	 * @param string $form_type		string used in form selector
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 */
	protected function checkChangeRemoveInheritedMacro($form_type, $host_type, $is_prototype = false, $lld_id = null) {
		if ($is_prototype) {
			$this->page->login()->open('host_prototypes.php?form=create&parent_discoveryid='.$lld_id);
			$form = $this->query('name:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();

			$name = 'Host prototype with edited global {#MACRO}';
			$form->fill([ucfirst($host_type).' name' => $name]);
			$form->selectTab('Groups');
			$form->fill(['Groups' => 'Zabbix servers']);
		}
		else {
			$this->page->login()->open($host_type.'s.php?form=create');
			$form = $this->query('name:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();

			$name = $host_type.' with edited global macro';
			$form->fill([
				ucfirst($host_type).' name' => $name,
				'Groups' => 'Zabbix servers'
			]);
		}
		$form->selectTab('Macros');
		// Go to inherited macros.
		$this->query('xpath://label[@for="show_inherited_macros_1"]')->waitUntilPresent()->one()->click();
		// Check inherited macros before changes.
		$this->checkInheritedGlobalMacros();

		$edited_macros = [
			[
				'macro' => '{$1}',
				'value' => 'New updated Numeric macro 1',
				'description' => 'New updated Test description 2'
			]
		];

		$count = count($edited_macros);
		// Change macro to edited values.
		for ($i = 0; $i < $count; $i += 1) {
			$this->query('id:macros_'.$i.'_change')->one()->click();
			$this->query('id:macros_'.$i.'_value')->one()->fill($edited_macros[$i]['value']);
			$this->query('id:macros_'.$i.'_description')->one()->fill($edited_macros[$i]['description']);
		}

		$form->submit();

		// Check saved edited macros in host/template form.
		$id = CDBHelper::getValue('SELECT hostid FROM hosts WHERE host='.zbx_dbstr($name));

		$this->page->open(
			$is_prototype
			? 'host_prototypes.php?form=update&parent_discoveryid='.$lld_id.'&hostid='.$id
			: $host_type.'s.php?form=update&'.$host_type.'id='.$id.'&groupid=0'
		);

		$form->selectTab('Macros');
		$this->assertMacros($edited_macros);

		// Remove edited macro and reset to global.
		$this->query('xpath://label[@for="show_inherited_macros_1"]')->waitUntilPresent()->one()->click();
		for ($i = 0; $i < $count; $i += 1) {
			$this->query('id:macros_'.$i.'_change')->waitUntilVisible()->one()->click();
		}
		$form->submit();

		$this->page->open(
			$is_prototype
			? 'host_prototypes.php?form=update&parent_discoveryid='.$lld_id.'&hostid='.$id
			: $host_type.'s.php?form=update&'.$host_type.'id='.$id.'&groupid=0'
		);

		$form->selectTab('Macros');
		$this->assertMacros();

		// Check inherited macros again after remove.
		$this->query('xpath://label[@for="show_inherited_macros_1"]')->waitUntilPresent()->one()->click();
		$this->checkInheritedGlobalMacros();
	}

	/**
	 *  Check adding and saving macros in host, host prototype or template form.
	 *
	 * @param array	$data			given data provider
	 * @param string $form_type		string used in form selector
	 * @param string $name			name of host where changes are made
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 */
	private function checkMacros($data = null, $form_type, $name, $host_type, $is_prototype, $lld_id) {
		if ($data['expected'] === TEST_BAD) {
			$old_hash = $this->getHash();
		}

		$form = $this->query('name:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();
		$form->selectTab('Macros');
		$this->fillMacros($data['macros']);
		$form->submit();

		$message = CMessageElement::find()->one();
		switch ($data['expected']) {
			case TEST_GOOD:
				$this->assertTrue($message->isGood());
				$this->assertEquals($data['success_message'], $message->getTitle());
				$this->assertEquals(1, CDBHelper::getCount('SELECT NULL FROM hosts WHERE host='.zbx_dbstr($name)));
				// Check the results in form.
				$this->checkMacrosFields($name, $is_prototype, $lld_id, $host_type, $form_type, $data);
				break;
			case TEST_BAD:
				$this->assertTrue($message->isBad());
				$this->assertEquals($data['error_message'], $message->getTitle());
				$this->assertTrue($message->hasLine($data['error_details']));
				// Check that DB hash is not changed.
				$this->assertEquals($old_hash, CDBHelper::getHash(self::SQL_HOSTS));
				break;
		}
	}

	/**
	 * Checking saved macros in host, host prototype or template form.
	 *
	 * @param string $name			name of host where changes are made
	 * @param boolean $is_prototype	defines is it prototype or not
	 * @param int $lld_id			points to LLD rule id where host prototype belongs
	 * @param string $host_type		string defining is it host, template or host prototype
	 * @param string $form_type		string used in form selector
	 * @param array	$data			given data provider
	 */
	private function checkMacrosFields($name, $is_prototype, $lld_id, $host_type, $form_type,  $data = null) {
		$id = CDBHelper::getValue('SELECT hostid FROM hosts WHERE host='.zbx_dbstr($name));

		$this->page->open(
			$is_prototype
			? 'host_prototypes.php?form=update&parent_discoveryid='.$lld_id.'&hostid='.$id
			: $host_type.'s.php?form=update&'.$host_type.'id='.$id.'&groupid=0'
		);

		$form = $this->query('id:'.$form_type.'Form')->waitUntilPresent()->asForm()->one();
		$form->selectTab('Macros');
		$this->assertMacros(($data !== null) ? $data['macros'] : []);
		$this->query('xpath://label[@for="show_inherited_macros_1"]')->waitUntilPresent()->one()->click();
		// Get all macros defined for this host.
		$hostmacros = CDBHelper::getAll('SELECT macro, value, description FROM hostmacro where hostid ='.$id);

		$this->checkInheritedGlobalMacros($hostmacros);
	}

	/**
	 * Check host/host prototype/template inherited macros in form matching with global macros in DB.
	 *
	 * @param array $hostmacros			all macros defined particularly for this host
	 */
	public function checkInheritedGlobalMacros($hostmacros = []) {
		// Create two macros arrays: from DB and from Frontend form.
		$macros = [
			// Merge global macros with host defined macros.
			'database' => array_merge(
					CDBHelper::getAll('SELECT macro, value, description FROM globalmacro'),
					$hostmacros
				),
			'frontend' => []
		];

		// Write macros rows from Frontend to array.
		$table = $this->query('id:tbl_macros')->waitUntilVisible()->asTable()->one();
		$count = $table->getRows()->count() - 1;
		for ($i = 0; $i < $count; $i += 2) {
			$macro = [];
			$row = $table->getRow($i);
			$macro['macro'] = $row->query('xpath:./td[1]/textarea')->one()->getValue();
			$macro['value'] = $row->query('xpath:./td[2]/div/textarea')->one()->getValue();
			$macro['description'] = $table->getRow($i + 1)->query('tag:textarea')->one()->getValue();

			$macros['frontend'][] = $macro;
		}

		// Sort arrays by Macros.
		foreach ($macros as &$array) {
			usort($array, function ($a, $b) {
				return strcmp($a['macro'], $b['macro']);
			});
		}
		unset($array);

		// Compare macros from DB with macros from Frontend.
		$this->assertEquals($macros['database'], $macros['frontend']);
	}

	public function createSecretMacros($macros, $url, $source) {
		$this->openMacrosTab($url, $source, true);
		// Check that macro values have type plain text by default.
		$this->assertEquals('Text', CInputGroupElement::find()->one()->getInputType());
		$this->fillMacros($macros);

		$row = 0;
		foreach ($macros as $macro) {
			$value_field = $this->query('xpath://textarea[@name="macros['.$row.'][macro]"]/../..//div['.
					CXPathHelper::fromClass('macro-value').']')->asInputGroup()->waitUntilVisible()->one();
			// Check that type of both macros is set to secret text.
			$this->assertEquals('Secret text', $value_field->getInputType());

			//Check that textarea input element is not available for secret text macros.
			$this->assertFalse($value_field->query('xpath:.//textarea[contains(@class, "textarea-flexible")]')
					->one(false)->isValid());

			$this->assertEquals($macro['value']['value'], $value_field->getValue());
			// Switch to tab with inherited and instance macros and verify that the value is secret but is still accessible.
			$this->checkInheritedTab($macro, true);
			// Check that macro value is hidden but is still accessible after swithing back to instance macros list.
			$value_field->isSecret();

			if ($macro['macro'] === '{$TEXT_MACRO}') {
				$value_field->changeInputType('Text');
			}
			$row++;
		}
		$this->query('button:Update')->one()->click();

		$this->openMacrosTab($url, $source, true);

		foreach ($macros as $macro) {
			$sql = 'SELECT value, description, type FROM hostmacro WHERE macro='.zbx_dbstr($macro['macro']);
			$type = ($macro['macro'] === '{$TEXT_MACRO}') ? 0 : 1;

			$value_field = $this->getValueField($macro['macro']);

			if ($macro['macro'] === '{$TEXT_MACRO}') {
				$this->assertEquals($macro['value']['value'], $value_field->getValue());
				$this->assertFalse($value_field->query('button:Set new value')->one(false)->isValid());
				$this->assertFalse($value_field->query('xpath:.//button[@title="Revert changes"]')->one(false)->isValid());

				// Switch to tab with inherited and instance macros and verify that the value is plain text.
				$this->checkInheritedTab($macro, false);
			}
			else {
				$this->assertEquals('******', $value_field->getValue());

				// Switch to tab with inherited and instance macros and verify that the value is secret and is not accessible.
				$this->checkInheritedTab($macro, true, false);

				$change_button = $value_field->query('button:Set new value')->one();
				$revert_button = $value_field->query('xpath:.//button[@title="Revert changes"]')->one();
				//Check that "Set new value" button is perent and "Revert" button is hidden if secret value wasn't modified.
				$this->assertTrue($change_button->isEnabled());
				$this->assertFalse($revert_button->isClickable());
				// Modify secret value and check that Revert button becomes clickable and "Set new value" button is Disabled.
				$change_button->click();
				$value_field->invalidate();
				$this->assertFalse($change_button->isEnabled());
				$this->assertTrue($revert_button->isClickable());
				// Revert changes
				$value_field->pressRevertButton();
			}
			$this->query('button:Update')->one()->click();
			// Check macro value, type and description in DB.
			$this->assertEquals([$macro['value']['value'], $macro['description'], $type], array_values(CDBHelper::getRow($sql)));
			$this->openMacrosTab($url, $source, true);
		}
	}

	public function updateSecretMacros($macros, $url, $source, $table) {
		$this->openMacrosTab($url, $source, true);
		$this->fillMacros($macros);

		foreach ($macros as $macro) {
			$value_field = $this->getValueField($macro['macro']);
			$secret = (CTestArrayHelper::get($macro['value'], 'type', 'Secret text') === 'Secret text') ? true : false;
			$this->checkInheritedTab($macro, $secret);
		}

		$this->query('button:Update')->one()->click();
		$this->openMacrosTab($url, $source);

		foreach ($macros as $macro) {
			$value_field = $this->getValueField($macro['macro']);
			if (CTestArrayHelper::get($macro['value'], 'type', 'Secret text') === 'Secret text') {
				$this->assertTrue($value_field->isSecret());
				$this->assertEquals('******', $value_field->getValue());
				$this->checkInheritedTab($macro, true, false);
			}
			else {
				$this->assertFalse($value_field->isSecret());
				$this->assertEquals($macro['value']['value'], $value_field->getValue());
				$this->checkInheritedTab($macro, false);
			}
			$sql = 'SELECT value FROM '.$table.'macro WHERE macro='.zbx_dbstr($macro['macro']);
			$this->assertEquals($macro['value']['value'], CDBHelper::getValue($sql));
		}
	}

	public function revertSecretMacroChanges($macros, $url, $source, $table) {
		$this->openMacrosTab($url, $source, true);

		$sql = 'SELECT * FROM '.$table.'macro WHERE macro in ('.CDBHelper::escape(array_column($macros, 'macro')).
				') ORDER BY macro';
		$old_hash = CDBHelper::getHash($sql);

		foreach ($macros as $macro) {
			$value_field = $this->getValueField($macro['macro']);
			// Check that the existing macro value is hidden.
			$this->assertEquals('******', $value_field->getValue());
			// Change the value of the secret macro to a unique value (absolute time)
			$value_field->fill([
				'value' => time()
			]);

			if ($macro['macro'] === '{$SECRET_HOST_MACRO_2_TEXT_REVERT}') {
				$value_field->changeInputType('Text');
			}
			// Press revert button amd save the changes acnd make sure that changes were reverted.
			$value_field->pressRevertButton();
		}
		$this->query('button:Update')->one()->click();
		// Check that no macro value changes took place.
		$this->assertEquals($old_hash, CDBHelper::getHash($sql));
	}

	public function resolveSecretMacro($macro, $url, $source) {
		$this->page->login()->open($url)->waitUntilReady();
		$this->query('link:Items')->one()->click();
		$this->page->waitUntilReady();

		$this->assertTrue($this->query('link', 'Macro value: '.$macro['value'])->one(false)->isValid());

		$this->openMacrosTab($url, $source);

		$value_field = $this->getValueField($macro['macro']);
		$value_field->changeInputType('Secret text');

		$this->query('button:Update')->one()->click();
		$this->openMacrosTab($url, $source);

		$this->query('link:Items')->one()->click();
		$this->page->waitUntilReady();

		$this->assertTrue($this->query('link', 'Macro value: ******')->one(false)->isValid());
	}


	public function getValueField($macro) {
		return $value_field = $this->query('xpath://textarea[text()="'.$macro.'"]/../..//div[contains(@class,"macro-value")]')
					->asInputGroup()->waitUntilVisible()->one();
	}

	public function checkInheritedTab($macro, $secret, $displayed = true) {
		// Switch to the list of inherited and instance macros.
		$this->query('xpath://label[@for="show_inherited_macros_1"]')->waitUntilPresent()->one()->click();
		$value_field = $this->getValueField($macro['macro']);

		if ($secret) {
			$this->assertTrue($value_field->isSecret());
			$expected_value = ($displayed) ? $macro['value']['value'] : '******';
			$this->assertEquals($expected_value, $value_field->getValue());
		}
		else {
			$this->assertFalse($value_field->isSecret());
			$this->assertEquals($macro['value']['value'], $value_field->getValue());
		}
		// Switch back to the list of instance macros.
		$this->query('xpath://label[@for="show_inherited_macros_0"]')->waitUntilPresent()->one()->click();
	}

	private function openMacrosTab($url, $source, $login = false){
		if ($login) {
			$this->page->login();
		}
		$this->page->open($url)->waitUntilReady();
		$this->query('id:'.$source.'Form')->asForm()->one()->selectTab('Macros');
	}

}
